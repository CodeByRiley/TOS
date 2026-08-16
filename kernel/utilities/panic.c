/* kernel/utilities/panic.c - fatal diagnostics and panic presentation. */
#include <arch/percpu.h>
#include <devices/pit.h>
#include <devices/serial.h>
#include <display/framebuffer.h>
#include <display/graphics.h>
#include <drivers/driver.h>
#include <interrupts/idt.h>
#include <sched/sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <utilities/panic.h>
#include <utilities/printf.h>

#define PANIC_BACKTRACE_MAX 16
#define AP_KSTACK_BYTES (16ULL * 1024)
#define PANIC_HOLD_MS 2000U

#define PANIC_BG 0x0025272Bu
#define PANIC_FG 0x00F2F2F3u
#define PANIC_MUTED 0x00B8BBC0u
#define PANIC_ACCENT 0x00E06C75u
#define PANIC_DIM 0x007A7F87u

static_assert(offsetof(struct interrupt_frame, rsp) ==
                  offsetof(struct interrupt_frame, rflags) + 8,
              "interrupt_frame rsp must immediately follow rflags");

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
static char screen_line[384];
static struct panic_record panic_records[MAX_CPUS];
static char exception_messages[MAX_CPUS][128];

extern char _kernel_start[];
extern char _kernel_end[];

static __attribute__((noreturn)) void panic_halt(void) {
  for (;;)
    __asm__ volatile("cli; hlt");
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

/*
 * Format an address as either "symbol+0xoff", "kernel+0xoff" (when inside
 * kernel text but unresolved), or the raw hex address.  Writes into the
 * caller-provided buffer so there are no allocations on the panic path.
 */
static void panic_symstr(char *buf, size_t bufsz, uint64_t address) {
  uint64_t off = 0;
  const char *sym = symtab_resolve(address, &off);

  if (sym)
    snprintf(buf, bufsz, "%s+0x%llx", sym, (unsigned long long)off);
  else if (kernel_address(address))
    snprintf(
        buf, bufsz, "kernel+0x%llx",
        (unsigned long long)(address - (uint64_t)(uintptr_t)_kernel_start));
  else
    snprintf(buf, bufsz, "0x%llx", (unsigned long long)address);
}

static void panic_address_line(const char *label, uint64_t address) {
  char symbuf[128];
  panic_symstr(symbuf, sizeof(symbuf), address);
  panic_serialf("%-18s %016llx  %s\n", label, (unsigned long long)address,
                symbuf);
}

static struct panic_machine_state panic_read_machine(void) {
  struct panic_machine_state s;
  __asm__ volatile("mov %%cr0, %0" : "=r"(s.cr0));
  __asm__ volatile("mov %%cr2, %0" : "=r"(s.cr2));
  __asm__ volatile("mov %%cr3, %0" : "=r"(s.cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(s.cr4));
  __asm__ volatile("pushfq; popq %0" : "=r"(s.rflags));
  return s;
}

static struct task *panic_task(int cpu_id) {
  /* Userspace scheduling is BSP-only. task_current() is global and would
   * misidentify an AP work callback as the task running on CPU 0. */
  return cpu_id == 0 ? task_current() : 0;
}

static int panic_stack_bounds(int cpu_id, struct task *task, uint64_t *low,
                              uint64_t *high) {
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
    if ((frame & 7) || frame < low || frame + 16 > high)
      break;

    const uint64_t *words = (const uint64_t *)(uintptr_t)frame;
    uint64_t next = words[0];
    uint64_t ret = words[1];

    char symbuf[128];
    panic_symstr(symbuf, sizeof(symbuf), ret);
    panic_serialf("  %02d: %016llx : %016llx  %s\n", i,
                  (unsigned long long)frame, (unsigned long long)ret, symbuf);
    emitted++;

    if (next <= frame || next + 16 > high || next - frame > 0x10000)
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
                (unsigned long long)f->rbp, (unsigned long long)frame_rsp(f),
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
  char caller_sym[128];
  panic_symstr(caller_sym, sizeof(caller_sym), record->caller);

  panic_serialf("panic(cpu %d caller %p): %s\n", record->cpu_id,
                (void *)(uintptr_t)record->caller,
                record->message ? record->message : "unspecified panic");
  panic_serialf("  caller resolved: %s\n", caller_sym);

  if (record->exception && record->frame) {
    panic_serialf("Exception: vector %llu (%s), error=%#llx\n",
                  (unsigned long long)record->frame->int_num, record->exception,
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

  panic_serialf("Kernel version: TOS x86_64 development (%s %s)\n", __DATE__,
                __TIME__);
  panic_serialf("Uptime ticks: %llu\n", (unsigned long long)pit_ticks());
  panic_serialf(
      "Control registers:\n"
      "  cr0=%016llx cr2=%016llx cr3=%016llx cr4=%016llx\n"
      "  rflags=%016llx\n",
      (unsigned long long)machine->cr0, (unsigned long long)machine->cr2,
      (unsigned long long)machine->cr3, (unsigned long long)machine->cr4,
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
  if (chars < 8)
    return;

  const char *p = text ? text : "unspecified panic";
  while (*p && *y + GFX_GLYPH_H * scale < surface->h) {
    int n = 0;
    int last_space = -1;
    while (p[n] && n < chars) {
      if (p[n] == ' ')
        last_space = n;
      n++;
    }
    if (p[n] && last_space > 0)
      n = last_space;
    if (n >= (int)sizeof(screen_line))
      n = sizeof(screen_line) - 1;
    for (int i = 0; i < n; i++)
      screen_line[i] = p[i];
    screen_line[n] = 0;
    screen_centered(surface, *y, screen_line, color, scale);
    *y += GFX_GLYPH_H * scale + 6;
    p += n;
    while (*p == ' ')
      p++;
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
        gfx_pixel(surface, cx + x, cy + y, PANIC_FG);
    }
  }
  gfx_fill(surface, gfx_rect_make(cx - 2, cy - 31, 5, 28), PANIC_FG);
}

static int panic_screen(const struct panic_record *record) {
  /* A secondary CPU may race the BSP's framebuffer worker. Keep the visual
   * presentation on CPU 0; every CPU still emits the full serial report. */
  if (record->cpu_id != 0 || !framebuffer_buffer())
    return 0;

  struct gfx_surface surface = framebuffer_get_gfx_surface();
  if (!surface.px || surface.w < 320 || surface.h < 240 ||
      surface.stride < surface.w)
    return 0;

  int headline_scale = surface.w >= 560 ? 2 : 1;

  gfx_clear(&surface, PANIC_BG);
  gfx_fill(&surface, gfx_rect_make(0, 0, surface.w, 5), PANIC_ACCENT);
  gfx_text(&surface, 24, 20, "TOS", PANIC_MUTED, 1);

  int icon_y = surface.h / 5;
  if (icon_y < 72)
    icon_y = 72;
  screen_power_symbol(&surface, surface.w / 2, icon_y);

  int y = icon_y + 58;
  screen_centered(&surface, y, "Your computer needs to restart.", PANIC_FG,
                  headline_scale);
  y += GFX_GLYPH_H * headline_scale + 18;
  screen_centered(&surface, y,
                  "TOS stopped because the kernel encountered a problem.",
                  PANIC_MUTED, 1);
  y += 28;
  screen_wrapped(&surface, &y, record->message, PANIC_FG, 1, surface.w - 64);

  int detail_y = surface.h - 68;
  if (detail_y > y + 12)
    y = detail_y;
  snprintf(screen_line, sizeof(screen_line), "panic(cpu %d caller %p)",
           record->cpu_id, (void *)(uintptr_t)record->caller);
  screen_centered(&surface, y, screen_line, PANIC_MUTED, 1);
  y += 16;
  screen_centered(&surface, y,
                  "A detailed diagnostic report is available on serial.",
                  PANIC_MUTED, 1);

  framebuffer_mark_damage(0, 0, (uint32_t)surface.w, (uint32_t)surface.h);
  framebuffer_present();
  return 1;
}

static const char *task_state_name(int state) {
  switch (state) {
  case TASK_RUNNING:
    return "RUNNING";
  case TASK_BLOCKED:
    return "BLOCKED";
  case TASK_ZOMBIE:
    return "ZOMBIE";
  case TASK_READY:
    return "READY";
  case TASK_DEAD:
    return "DEAD";
  case TASK_SLEEPING:
    return "SLEEP";
  case TASK_LOADING:
    return "LOADING";
  default:
    return "UNKNOWN";
  }
}

static const char *device_bus_name(int bus) {
  switch (bus) {
  case DEVICE_BUS_PCI:
    return "PCI";
  case DEVICE_BUS_ISA:
    return "ISA";
  default:
    return "NONE";
  }
}

static uint64_t panic_stop_code(const struct panic_record *record) {
  if (record->frame)
    return record->frame->int_num & 0xFFFFFFFFu;
  return 0x000000FFu;
}

static void diag_line(struct gfx_surface *surface, int *y, int x,
                      const char *text, uint32_t color) {
  const int line_h = GFX_GLYPH_H + 2;
  if (!text || *y + line_h > surface->h)
    return;

  int max_chars = (surface->w - x - 8) / GFX_GLYPH_W;
  if (max_chars <= 0)
    return;
  gfx_text_n(surface, x, *y, text, (size_t)max_chars, color, 1);
  *y += line_h;
}

static void diag_rule(struct gfx_surface *surface, int *y) {
  if (*y + 4 >= surface->h)
    return;
  gfx_fill(surface, gfx_rect_make(8, *y, surface->w - 16, 1), PANIC_ACCENT);
  *y += 6;
}

static void diag_hex2(struct gfx_surface *surface, int *y,
                      const char *left_label, uint64_t left,
                      const char *right_label, uint64_t right) {
  snprintf(screen_line, sizeof(screen_line), "%-6s %016llx    %-6s %016llx",
           left_label, (unsigned long long)left, right_label,
           (unsigned long long)right);
  diag_line(surface, y, 8, screen_line, PANIC_FG);
}

static void diag_registers(struct gfx_surface *surface, int *y,
                           const struct panic_record *record,
                           const struct panic_machine_state *machine) {
  diag_line(surface, y, 8, "CPU register dump", PANIC_MUTED);
  diag_hex2(surface, y, "CR0", machine->cr0, "CR2", machine->cr2);
  diag_hex2(surface, y, "CR3", machine->cr3, "CR4", machine->cr4);
  diag_hex2(surface, y, "RFL", machine->rflags, "RIP", record->caller);

  if (!record->frame) {
    diag_hex2(surface, y, "RSP", record->rsp, "RBP", record->rbp);
    return;
  }

  const struct interrupt_frame *f = record->frame;
  diag_hex2(surface, y, "RAX", f->rax, "RBX", f->rbx);
  diag_hex2(surface, y, "RCX", f->rcx, "RDX", f->rdx);
  diag_hex2(surface, y, "RSI", f->rsi, "RDI", f->rdi);
  diag_hex2(surface, y, "RBP", f->rbp, "RSP", frame_rsp(f));
  diag_hex2(surface, y, "R8", f->r8, "R9", f->r9);
  diag_hex2(surface, y, "R10", f->r10, "R11", f->r11);
  diag_hex2(surface, y, "R12", f->r12, "R13", f->r13);
  diag_hex2(surface, y, "R14", f->r14, "R15", f->r15);
  diag_hex2(surface, y, "CS", f->cs, "SS", (f->cs & 3) == 3 ? f->ss : 0);
}

static void diag_tasks(struct gfx_surface *surface, int *y) {
  struct task_snap snaps[MAX_TASKS];
  int count = sched_snapshot(snaps, MAX_TASKS);

  diag_line(surface, y, 8, "Task table", PANIC_MUTED);
  diag_line(surface, y, 8, "PID   PPID  STATE     TICKS              NAME",
            PANIC_DIM);
  for (int i = 0; i < count && *y + GFX_GLYPH_H < surface->h; i++) {
    snprintf(screen_line, sizeof(screen_line),
             "%04d  %04d  %-8s  %016llx   %-15s", snaps[i].pid,
             snaps[i].parent_pid, task_state_name(snaps[i].state),
             (unsigned long long)snaps[i].ticks_run,
             snaps[i].name[0] ? snaps[i].name : "unnamed");
    diag_line(surface, y, 8, screen_line, PANIC_FG);
  }
}

static void diag_drivers(struct gfx_surface *surface, int *y) {
  struct driver_snap snaps[DRIVER_SNAP_MAX];
  int count = driver_snapshot(snaps, DRIVER_SNAP_MAX);

  diag_line(surface, y, 8, "Kernel drivers", PANIC_MUTED);
  snprintf(screen_line, sizeof(screen_line),
           "%-4s  %-4s  %-4s   %-5s    %-8s %s", "IDX", "BUS", "POLL", "BOUND",
           "ENABLED", "NAME");
  diag_line(surface, y, 8, screen_line, PANIC_DIM);
  for (int i = 0; i < count && *y + GFX_GLYPH_H < surface->h; i++) {
    snprintf(screen_line, sizeof(screen_line),
             "%04d  %-4s  %-4s   %05u    %-8s %-31s", i,
             device_bus_name(snaps[i].bus), snaps[i].poll ? "yes" : "no",
             snaps[i].bound_devices, snaps[i].enabled ? "yes" : "no",
             snaps[i].name[0] ? snaps[i].name : "unnamed");
    diag_line(surface, y, 8, screen_line, PANIC_FG);
  }
}

static void diag_stack(struct gfx_surface *surface, int *y,
                       const struct panic_record *record) {
  struct task *task = panic_task(record->cpu_id);
  uint64_t low, high;
  uint64_t frame = record->rbp;

  diag_line(surface, y, 8, "Stack frames", PANIC_MUTED);
  diag_line(surface, y, 8, "ADDRESS             NEXT               RETURN",
            PANIC_DIM);

  if (!frame || !panic_stack_bounds(record->cpu_id, task, &low, &high)) {
    diag_line(surface, y, 8, "unavailable - no trusted kernel stack bounds",
              PANIC_DIM);
    return;
  }

  int emitted = 0;
  for (int i = 0; i < 10 && *y + GFX_GLYPH_H < surface->h; i++) {
    if ((frame & 7) || frame < low || frame + 16 > high)
      break;

    const uint64_t *words = (const uint64_t *)(uintptr_t)frame;
    uint64_t next = words[0];
    uint64_t ret = words[1];
    char symbuf[128];
    panic_symstr(symbuf, sizeof(symbuf), ret);
    snprintf(screen_line, sizeof(screen_line),
             "%016llx  %016llx  %016llx  %s",
             (unsigned long long)frame,
             (unsigned long long)next,
             (unsigned long long)ret,
             symbuf);
    diag_line(surface, y, 8, screen_line,
              kernel_address(ret) ? PANIC_FG : PANIC_MUTED);
    emitted++;

    if (next <= frame || next + 16 > high || next - frame > 0x10000)
      break;
    frame = next;
  }

  if (!emitted)
    diag_line(surface, y, 8, "unavailable - frame pointer outside kernel stack",
              PANIC_DIM);
}

static void panic_diagnostic_screen(const struct panic_record *record,
                                    const struct panic_machine_state *machine) {
  if (record->cpu_id != 0 || !framebuffer_buffer())
    return;

  struct gfx_surface surface = framebuffer_get_gfx_surface();
  if (!surface.px || surface.w < 320 || surface.h < 240 ||
      surface.stride < surface.w)
    return;

  gfx_clear(&surface, PANIC_BG);

  int y = 8;
  snprintf(screen_line, sizeof(screen_line), "*** STOP: 0x%08llx (%s)",
           (unsigned long long)panic_stop_code(record),
           record->exception ? record->exception : "TOS_KERNEL_PANIC");
  diag_line(&surface, &y, 8, screen_line, PANIC_FG);

  snprintf(screen_line, sizeof(screen_line),
           "TOS_KERNEL_PANIC: Address %016llx base at %016llx - kernel",
           (unsigned long long)record->caller,
           (unsigned long long)(uintptr_t)_kernel_start);
  diag_line(&surface, &y, 8, screen_line, PANIC_FG);

  snprintf(screen_line, sizeof(screen_line),
           "*** Address %016llx has base at %016llx",
           (unsigned long long)record->caller,
           (unsigned long long)(uintptr_t)_kernel_start);
  diag_line(&surface, &y, 8, screen_line, PANIC_FG);
  diag_rule(&surface, &y);

  snprintf(screen_line, sizeof(screen_line),
           "CPU %d: TOS x86_64 development  uptime ticks %llu", record->cpu_id,
           (unsigned long long)pit_ticks());
  diag_line(&surface, &y, 8, screen_line, PANIC_MUTED);

  if (record->file) {
    snprintf(screen_line, sizeof(screen_line), "Source: %s:%d in %s()",
             record->file, record->line,
             record->func ? record->func : "unknown");
    diag_line(&surface, &y, 8, screen_line, PANIC_MUTED);
  }
  diag_line(&surface, &y, 8,
            record->message ? record->message : "unspecified panic", PANIC_FG);
  diag_rule(&surface, &y);

  diag_registers(&surface, &y, record, machine);
  diag_rule(&surface, &y);
  diag_drivers(&surface, &y);
  diag_rule(&surface, &y);
  diag_tasks(&surface, &y);
  diag_rule(&surface, &y);
  diag_stack(&surface, &y, record);

  int bottom = surface.h - (GFX_GLYPH_H + 8);
  if (bottom > y) {
    y = bottom;
    diag_line(&surface, &y, 8, "Beginning dump of kernel diagnostic data",
              PANIC_MUTED);
  }

  framebuffer_mark_damage(0, 0, (uint32_t)surface.w, (uint32_t)surface.h);
  framebuffer_present();
}

static __attribute__((noreturn)) void
panic_finish(struct panic_record *record) {
  __asm__ volatile("cli");

  int expected = -1;
  if (!__atomic_compare_exchange_n(&panic_owner, &expected, record->cpu_id, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    panic_serialf("\nDOUBLE PANIC on cpu %d while panic owner is cpu %d\n",
                  record->cpu_id, expected);
    // serial_write_str(" while panic owner is cpu ");
    // serial_write_hex((uint64_t)(uint32_t)expected);
    panic_halt();
  }

  struct panic_machine_state machine = panic_read_machine();
  panic_serial_report(record, &machine);
  if (panic_screen(record))
    pit_delay_ms(PANIC_HOLD_MS);
  panic_diagnostic_screen(record, &machine);
  panic_halt();
}

void panic_at(const char *msg, const char *file, int line, const char *func) {
  uint64_t rsp;
  __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
  int cpu_id = percpu_current_id();
  if (cpu_id < 0 || cpu_id >= MAX_CPUS)
    cpu_id = 0;
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

void panic_from_exception(const char *name, const struct interrupt_frame *frame,
                          uint64_t fault_address, int has_fault_address) {
  int cpu_id = percpu_current_id();
  if (cpu_id < 0 || cpu_id >= MAX_CPUS)
    cpu_id = 0;
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
      .rbp =
          (uint64_t)(uintptr_t)__builtin_frame_address(0), // Trace kernel path
      .rsp = frame ? frame_rsp(frame) : 0,
      .fault_address = fault_address,
      .has_fault_address = has_fault_address,
  };
  panic_finish(record);
}
