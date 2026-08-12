/* kernel/utilities/panic.c - fatal diagnostics and panic presentation. */
#include "utilities/panic.h"
#include "arch/percpu.h"
#include "devices/pit.h"
#include "devices/serial.h"
#include "display/graphics.h"
#include "display/framebuffer.h"
#include "interrupts/idt.h"
#include "sched/sched.h"
#include "utilities/printf.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define PANIC_BACKTRACE_MAX 16
#define AP_KSTACK_BYTES     (16ULL * 1024)

struct panic_record {
    const char *message;
    const char *file;
    const char *func;
    const char *exception;
    const struct interrupt_frame *frame;
    int line;
    int cpu_id;
    uint64_t caller;
    uint64_t rbp;
    uint64_t rsp;
    uint64_t fault_address;
    int has_fault_address;
};

struct panic_machine_state {
    uint64_t cr0, cr2, cr3, cr4, rflags;
};

static volatile int panic_owner = -1;
static char serial_line[512];
static char screen_line[192];
static struct panic_record panic_records[MAX_CPUS];
static char exception_messages[MAX_CPUS][128];

extern char _kernel_start[];
extern char _kernel_end[];

static __attribute__((noreturn)) void panic_halt(void) {
    for (;;)
        __asm__ volatile ("cli; hlt");
}

static void panic_serialf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(serial_line, sizeof(serial_line), fmt, ap);
    va_end(ap);
    serial_write_str(serial_line);
}

static int kernel_address(uint64_t address) {
    uint64_t start = (uint64_t)(uintptr_t)_kernel_start;
    uint64_t end = (uint64_t)(uintptr_t)_kernel_end;
    return address >= start && address < end;
}

static void panic_address_line(const char *label, uint64_t address) {
    panic_serialf("%-18s ", label);
    serial_write_hex(address);
    if (kernel_address(address)) {
        serial_write_str("  kernel+");
        serial_write_hex(address - (uint64_t)(uintptr_t)_kernel_start);
    }
    serial_write_str("\n");
}

static struct panic_machine_state panic_read_machine(void) {
    struct panic_machine_state s;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(s.cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(s.cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(s.cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(s.cr4));
    __asm__ volatile ("pushfq; popq %0" : "=r"(s.rflags));
    return s;
}

static struct task *panic_task(int cpu_id) {
    /* Userspace scheduling is BSP-only. task_current() is global and would
     * misidentify an AP work callback as the task running on CPU 0. */
    return cpu_id == 0 ? task_current() : 0;
}

static int panic_stack_bounds(int cpu_id, struct task *task,
                              uint64_t *low, uint64_t *high) {
    if (cpu_id == 0 && task && task->kstack && task->syscall_kstack_top) {
        *low = (uint64_t)(uintptr_t)task->kstack;
        *high = task->syscall_kstack_top;
        return 1;
    }

    struct cpu_local *cpu = percpu_get(cpu_id);
    if (cpu_id > 0 && cpu && cpu->kernel_rsp_top) {
        *high = cpu->kernel_rsp_top;
        *low = *high - AP_KSTACK_BYTES;
        return 1;
    }
    return 0;
}

static void panic_backtrace(const struct panic_record *record,
                            struct task *task) {
    uint64_t low, high;
    serial_write_str("Backtrace (frame : return address):\n");
    if (!record->rbp || !panic_stack_bounds(record->cpu_id, task, &low, &high)) {
        serial_write_str("  unavailable (no trusted kernel stack bounds)\n");
        return;
    }

    uint64_t frame = record->rbp;
    int emitted = 0;
    for (int i = 0; i < PANIC_BACKTRACE_MAX; i++) {
        if ((frame & 7) || frame < low || frame > high - 16)
            break;

        const uint64_t *words = (const uint64_t *)(uintptr_t)frame;
        uint64_t next = words[0];
        uint64_t ret = words[1];
        panic_serialf("  %02d: ", i);
        serial_write_hex(frame);
        serial_write_str(" : ");
        serial_write_hex(ret);
        if (kernel_address(ret)) {
            serial_write_str("  kernel+");
            serial_write_hex(ret - (uint64_t)(uintptr_t)_kernel_start);
        }
        serial_write_str("\n");
        emitted++;

        if (next <= frame || next > high - 16 || next - frame > 0x10000)
            break;
        frame = next;
    }
    if (!emitted)
        serial_write_str("  unavailable (frame pointer outside kernel stack)\n");
}

static uint64_t frame_rsp(const struct interrupt_frame *frame) {
    if ((frame->cs & 3) == 3)
        return frame->rsp;
    return (uint64_t)(uintptr_t)&frame->rsp;
}

static void panic_report_registers(const struct interrupt_frame *f) {
    panic_serialf("Registers:\n"
                  "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
                  "  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
                  "   r8=%016llx  r9=%016llx r10=%016llx r11=%016llx\n"
                  "  r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n"
                  "   cs=%016llx  ss=%016llx flags=%016llx\n",
                  (unsigned long long)f->rax, (unsigned long long)f->rbx,
                  (unsigned long long)f->rcx, (unsigned long long)f->rdx,
                  (unsigned long long)f->rsi, (unsigned long long)f->rdi,
                  (unsigned long long)f->rbp,
                  (unsigned long long)frame_rsp(f),
                  (unsigned long long)f->r8, (unsigned long long)f->r9,
                  (unsigned long long)f->r10, (unsigned long long)f->r11,
                  (unsigned long long)f->r12, (unsigned long long)f->r13,
                  (unsigned long long)f->r14, (unsigned long long)f->r15,
                  (unsigned long long)f->cs,
                  (unsigned long long)((f->cs & 3) == 3 ? f->ss : 0),
                  (unsigned long long)f->rflags);
}

static void panic_serial_report(const struct panic_record *record,
                                const struct panic_machine_state *machine) {
    struct task *task = panic_task(record->cpu_id);

    serial_write_str("\n*** TOS KERNEL PANIC ***\n");
    panic_serialf("panic(cpu %d caller %p): %s\n", record->cpu_id,
                  (void *)(uintptr_t)record->caller,
                  record->message ? record->message : "unspecified panic");

    if (record->exception) {
        panic_serialf("Exception: vector %llu (%s), error=%#llx\n",
                      (unsigned long long)record->frame->int_num,
                      record->exception,
                      (unsigned long long)record->frame->err_code);
    }
    if (record->file) {
        panic_serialf("Source: %s:%d in %s()\n", record->file, record->line,
                      record->func ? record->func : "unknown");
    }
    if (record->has_fault_address)
        panic_address_line("Fault address:", record->fault_address);
    panic_address_line("Instruction:", record->caller);

    if (task) {
        panic_serialf("Panicked task: pid %d, name %s, state %d\n", task->pid,
                      task->name[0] ? task->name : "unnamed", task->state);
    } else if (record->cpu_id > 0) {
        serial_write_str("Panicked context: AP kernel worker\n");
    } else {
        serial_write_str("Panicked task: scheduler not initialized\n");
    }

    panic_serialf("Kernel version: TOS x86_64 development (%s %s)\n",
                  __DATE__, __TIME__);
    panic_serialf("Uptime ticks: %llu\n",
                  (unsigned long long)pit_ticks());
    panic_serialf("Control registers:\n"
                  "  cr0=%016llx cr2=%016llx cr3=%016llx cr4=%016llx\n"
                  "  rflags=%016llx\n",
                  (unsigned long long)machine->cr0,
                  (unsigned long long)machine->cr2,
                  (unsigned long long)machine->cr3,
                  (unsigned long long)machine->cr4,
                  (unsigned long long)machine->rflags);

    if (record->frame)
        panic_report_registers(record->frame);
    else
        panic_serialf("Context: rsp=%016llx rbp=%016llx\n",
                      (unsigned long long)record->rsp,
                      (unsigned long long)record->rbp);

    panic_backtrace(record, task);
    serial_write_str("Kernel text: ");
    serial_write_hex((uint64_t)(uintptr_t)_kernel_start);
    serial_write_str(" - ");
    serial_write_hex((uint64_t)(uintptr_t)_kernel_end);
    serial_write_str("\nPlease restart the machine.\n");
}

static void screen_centered(struct gfx_surface *surface, int y,
                            const char *text, uint32_t color, int scale) {
    int w = 0;
    gfx_text_size(text, scale, &w, 0);
    gfx_text(surface, (surface->w - w) / 2, y, text, color, scale);
}

static void screen_wrapped(struct gfx_surface *surface, int *y,
                           const char *text, uint32_t color, int scale,
                           int max_width) {
    int chars = max_width / (GFX_GLYPH_W * scale);
    if (chars < 8) return;

    const char *p = text ? text : "unspecified panic";
    while (*p && *y + GFX_GLYPH_H * scale < surface->h) {
        int n = 0;
        int last_space = -1;
        while (p[n] && n < chars) {
            if (p[n] == ' ') last_space = n;
            n++;
        }
        if (p[n] && last_space > 0) n = last_space;
        if (n >= (int)sizeof(screen_line)) n = sizeof(screen_line) - 1;
        for (int i = 0; i < n; i++) screen_line[i] = p[i];
        screen_line[n] = 0;
        screen_centered(surface, *y, screen_line, color, scale);
        *y += GFX_GLYPH_H * scale + 6;
        p += n;
        while (*p == ' ') p++;
    }
}

static void screen_power_symbol(struct gfx_surface *surface, int cx, int cy) {
    const int radius = 24;
    const int inner = 20;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            int d = x * x + y * y;
            int top_gap = y < -7 && x > -9 && x < 9;
            if (!top_gap && d <= radius * radius && d >= inner * inner)
                gfx_pixel(surface, cx + x, cy + y, 0x00E8E8EA);
        }
    }
    gfx_fill(surface, gfx_rect_make(cx - 2, cy - 31, 5, 28), 0x00E8E8EA);
}

static void panic_screen(const struct panic_record *record) {
    /* A secondary CPU may race the BSP's framebuffer worker. Keep the visual
     * presentation on CPU 0; every CPU still emits the full serial report. */
    if (record->cpu_id != 0 || !framebuffer_buffer())
        return;

    struct gfx_surface surface = framebuffer_get_gfx_surface();
    if (!surface.px || surface.w < 320 || surface.h < 240 ||
        surface.stride < surface.w)
        return;

    const uint32_t bg = 0x0025272B;
    const uint32_t fg = 0x00F2F2F3;
    const uint32_t muted = 0x00B8BBC0;
    const uint32_t accent = 0x00E06C75;
    int headline_scale = surface.w >= 560 ? 2 : 1;

    gfx_clear(&surface, bg);
    gfx_fill(&surface, gfx_rect_make(0, 0, surface.w, 5), accent);
    gfx_text(&surface, 24, 20, "TOS", muted, 1);

    int icon_y = surface.h / 5;
    if (icon_y < 72) icon_y = 72;
    screen_power_symbol(&surface, surface.w / 2, icon_y);

    int y = icon_y + 58;
    screen_centered(&surface, y, "Your computer needs to restart.", fg,
                    headline_scale);
    y += GFX_GLYPH_H * headline_scale + 18;
    screen_centered(&surface, y,
                    "TOS stopped because the kernel encountered a problem.",
                    muted, 1);
    y += 28;
    screen_wrapped(&surface, &y, record->message, fg, 1, surface.w - 64);

    int detail_y = surface.h - 68;
    if (detail_y > y + 12) y = detail_y;
    snprintf(screen_line, sizeof(screen_line),
             "panic(cpu %d caller %p)", record->cpu_id,
             (void *)(uintptr_t)record->caller);
    screen_centered(&surface, y, screen_line, muted, 1);
    y += 16;
    screen_centered(&surface, y,
                    "A detailed diagnostic report is available on serial.",
                    muted, 1);

    framebuffer_mark_damage(0, 0, (uint32_t)surface.w, (uint32_t)surface.h);
    framebuffer_present();
}

static __attribute__((noreturn)) void panic_finish(
    struct panic_record *record) {
    __asm__ volatile ("cli");

    int expected = -1;
    if (!__atomic_compare_exchange_n(&panic_owner, &expected, record->cpu_id,
                                     0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        serial_write_str("\nDOUBLE PANIC on cpu ");
        serial_write_hex((uint64_t)(uint32_t)record->cpu_id);
        serial_write_str(" while panic owner is cpu ");
        serial_write_hex((uint64_t)(uint32_t)expected);
        serial_write_str("\n");
        panic_halt();
    }

    struct panic_machine_state machine = panic_read_machine();
    panic_serial_report(record, &machine);
    panic_screen(record);
    panic_halt();
}

void panic_at(const char *msg, const char *file, int line, const char *func) {
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    int cpu_id = percpu_current_id();
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) cpu_id = 0;
    struct panic_record *record = &panic_records[cpu_id];
    *record = (struct panic_record){
        .message = msg,
        .file = file,
        .func = func,
        .line = line,
        .cpu_id = cpu_id,
        .caller = (uint64_t)(uintptr_t)__builtin_return_address(0),
        .rbp = (uint64_t)(uintptr_t)__builtin_frame_address(0),
        .rsp = rsp,
    };
    panic_finish(record);
}

void panic_from_exception(const char *name,
                          const struct interrupt_frame *frame,
                          uint64_t fault_address,
                          int has_fault_address) {
    int cpu_id = percpu_current_id();
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) cpu_id = 0;
    char *exception_message = exception_messages[cpu_id];
    snprintf(exception_message, sizeof(exception_messages[cpu_id]),
             "unhandled CPU exception: %s", name ? name : "unknown");

    struct panic_record *record = &panic_records[cpu_id];
    *record = (struct panic_record){
        .message = exception_message,
        .exception = name ? name : "unknown",
        .frame = frame,
        .cpu_id = cpu_id,
        .caller = frame ? frame->rip : 0,
        .rbp = frame && (frame->cs & 3) == 0 ? frame->rbp : 0,
        .rsp = frame ? frame_rsp(frame) : 0,
        .fault_address = fault_address,
        .has_fault_address = has_fault_address,
    };
    panic_finish(record);
}
