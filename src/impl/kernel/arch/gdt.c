/* src/impl/kernel/arch/gdt.c — long-mode GDT + per-CPU TSS.
 *
 * GDT layout:
 *   0x00: null
 *   0x08: kernel code  (8 bytes)
 *   0x10: kernel data  (8 bytes)
 *   0x18: user data    (8 bytes)
 *   0x20: user code    (8 bytes)
 *   0x28..0x28+16*N-1: per-CPU TSS descriptors (16 bytes each, N=MAX_CPUS)
 *
 * Total = 5*8 + 16*MAX_CPUS bytes (168 bytes at MAX_CPUS=8). Each CPU's
 * TSS lives in tss_table[cpu_id]; the matching GDT descriptor is
 * installed by gdt_install_tss() and loaded into TR by
 * gdt_load_tss_this_cpu().
 */
#include "arch/gdt.h"
#include "arch/percpu.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stddef.h>
#include <stdint.h>

struct __attribute__((packed)) gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;     // limit[19:16] in low nibble, flags in high nibble
    uint8_t  base_high;
};

struct __attribute__((packed)) tss_desc {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

_Static_assert(sizeof(struct gdt_entry) == 8,
               "GDT descriptor must be 8 bytes");
_Static_assert(sizeof(struct tss_desc) == 16,
               "64-bit TSS descriptor must be 16 bytes");
_Static_assert(sizeof(struct gdtr) == 10,
               "GDTR operand must be 10 bytes in long mode");
_Static_assert(sizeof(struct tss) == 104,
               "64-bit TSS body must use the architectural 104-byte layout");
_Static_assert(offsetof(struct tss, rsp0) == 4,
               "TSS.rsp0 offset is architectural");
_Static_assert(offsetof(struct tss, iomap_base) == 102,
               "TSS.iomap_base offset is architectural");

/* GDT layout:
 *   0x00: null
 *   0x08: kernel code  (8 bytes)
 *   0x10: kernel data  (8 bytes)
 *   0x18: user data    (8 bytes)
 *   0x20: user code    (8 bytes)
 *   0x28..0x28+16*N-1: per-CPU TSS descriptors (16 bytes each, N = MAX_CPUS)
 *
 * Total size = 5*8 + 16*MAX_CPUS bytes. With MAX_CPUS=8 -> 168 bytes.
 * Each CPU's TSS lives in tss_table[cpu_id]; the matching GDT descriptor
 * is installed by gdt_install_tss() and loaded into TR by gdt_load_tss_this_cpu(). */

#define GDT_FIXED_BYTES   (5 * 8)
#define GDT_TOTAL_BYTES   (GDT_FIXED_BYTES + 16 * MAX_CPUS)

static uint8_t  gdt_table[GDT_TOTAL_BYTES] __attribute__((aligned(8)));
static struct tss tss_table[MAX_CPUS] __attribute__((aligned(16)));
static struct gdtr gdtr;
static uint8_t  bsp_kernel_stack[16384] __attribute__((aligned(16)));

/* Dedicated per-CPU double-fault stacks, wired to IST1.
 *
 * Without these a #DF is delivered on whatever stack was already broken —
 * so the CPU cannot push the exception frame, and a triple fault reboots
 * the machine with no output at all. An IST entry makes the CPU switch to
 * a known-good stack unconditionally, which turns a silent reset into a
 * printable report. Kept small: the handler logs and halts, it does not
 * return or recurse. */
#define DF_STACK_BYTES 4096
static uint8_t df_stacks[MAX_CPUS][DF_STACK_BYTES] __attribute__((aligned(16)));

static void set_entry(uint16_t selector, uint8_t access, uint8_t flags) {
    struct gdt_entry *e = (struct gdt_entry*)&gdt_table[selector];
    e->limit_low   = 0xFFFF;
    e->base_low    = 0;
    e->base_mid    = 0;
    e->access      = access;
    e->granularity = 0x0F | (flags << 4);
    e->base_high   = 0;
}

static void set_tss(uint16_t selector, uint64_t base, uint32_t limit) {
    struct tss_desc *t = (struct tss_desc*)&gdt_table[selector];
    t->limit_low   = limit & 0xFFFF;
    t->base_low    = base & 0xFFFF;
    t->base_mid    = (base >> 16) & 0xFF;
    t->access      = 0x89;                  // available 64-bit TSS
    t->granularity = (limit >> 16) & 0x0F;
    t->base_high   = (base >> 24) & 0xFF;
    t->base_upper  = (uint32_t)(base >> 32);
    t->reserved    = 0;
}

void gdt_load_this_cpu_full(void) {
    __asm__ volatile (
        "lgdt %0                 \n"
        "pushq $0x08             \n"
        "leaq 1f(%%rip), %%rax   \n"
        "pushq %%rax             \n"
        "lretq                   \n"
        "1:                      \n"
        "mov $0x10, %%ax         \n"
        "mov %%ax, %%ds          \n"
        "mov %%ax, %%es          \n"
        "mov %%ax, %%fs          \n"
        "mov %%ax, %%gs          \n"
        "mov %%ax, %%ss          \n"
        :
        : "m"(gdtr)
        : "rax", "memory"
    );
}

void gdt_load_this_cpu(void) {
    gdt_load_this_cpu_full();
}

void gdt_init(void) {
    memset(gdt_table, 0, sizeof(gdt_table));        // null at 0x00
    set_entry(GDT_KERNEL_CODE, 0x9A, 0xA);          // kernel code, 64-bit
    set_entry(GDT_KERNEL_DATA, 0x92, 0xC);          // kernel data
    set_entry(GDT_USER_DATA,   0xF2, 0xC);          // user data, DPL=3
    set_entry(GDT_USER_CODE,   0xFA, 0xA);          // user code, DPL=3, 64-bit

    memset(tss_table, 0, sizeof(tss_table));

    /* Install BSP TSS at GDT_TSS using a static boot stack — sched will
     * overwrite rsp0 once the BSP gets a real per-task syscall stack. */
    gdt_install_tss(0, (uint64_t)(bsp_kernel_stack + sizeof(bsp_kernel_stack)));

    gdtr.limit = sizeof(gdt_table) - 1;
    gdtr.base  = (uint64_t)gdt_table;

    gdt_load_this_cpu_full();

    /* Load BSP TR. APs load their own via gdt_load_tss_this_cpu(cpu_id). */
    gdt_load_tss_this_cpu(0);

    log_write("GDT + TSS loaded", KERNEL, LOG_INFO);
}

void gdt_install_tss(int cpu_id, uint64_t kstack_top) {
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return;
    struct tss *t = &tss_table[cpu_id];
    memset(t, 0, sizeof(*t));
    t->iomap_base = sizeof(*t);     // no I/O permission map
    t->rsp0       = kstack_top;
    /* IST1 = double fault. The IDT points vector 8 at this slot, so #DF is
     * always delivered on a stack that is known good even when rsp0 is the
     * thing that broke. */
    t->ist1       = (uint64_t)(df_stacks[cpu_id] + DF_STACK_BYTES);
    set_tss((uint16_t)GDT_TSS_FOR(cpu_id), (uint64_t)t, sizeof(*t) - 1);
}

void gdt_load_tss_this_cpu(int cpu_id) {
    uint16_t sel = (uint16_t)GDT_TSS_FOR(cpu_id);
    /* Once a TSS has been loaded into TR, its descriptor's type byte flips
     * from 0x89 (available) to 0x8B (busy). Re-issuing ltr on a busy
     * descriptor triggers #GP. Clear bit 1 (busy) back to 0x89 so this
     * function is safe to call repeatedly — including the BSP path that
     * re-loads its own TSS after percpu_init_bsp wires up the pointer. */
    gdt_table[sel + 5] = 0x89;
    __asm__ volatile ("ltr %0" :: "r"(sel) : "memory");
    struct cpu_local *c = percpu_get(cpu_id);
    if (c) c->tss = &tss_table[cpu_id];
}

void tss_set_rsp0(uint64_t rsp0) {
    /* Legacy single-CPU API. Routes to CPU 0 for backward compat with old
     * callers (sched stage_for now uses tss_set_rsp0_for explicitly). */
    tss_table[0].rsp0 = rsp0;
}

void tss_set_rsp0_for(int cpu_id, uint64_t rsp0) {
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return;
    tss_table[cpu_id].rsp0 = rsp0;
}
