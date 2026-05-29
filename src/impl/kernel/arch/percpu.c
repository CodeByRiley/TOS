#include "arch/percpu.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

/* Per-CPU data implementation. One struct per logical CPU, GS_BASE on
 * each core points at its own slot. The BSP populates its slot during
 * early kernel_main; each AP populates its slot from inside ap_entry()
 * before it touches anything else that uses gs-relative addressing. */

#define MSR_GS_BASE         0xC0000101u
#define MSR_KERNEL_GS_BASE  0xC0000102u

static struct cpu_local cpus[MAX_CPUS] __attribute__((aligned(64)));
static int              cpu_count = 1;

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static void set_gs_base(struct cpu_local *c) {
    /* Set both GS_BASE and KERNEL_GS_BASE to the same value. We don't use
     * SWAPGS (userspace doesn't touch gs), but staging the same pointer
     * into both means a stray SWAPGS — including one we add later — won't
     * leave GS pointing into oblivion. */
    wrmsr(MSR_GS_BASE,        (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)c);
}

void percpu_init_bsp(uint8_t bsp_lapic_id) {
    memset(cpus, 0, sizeof(cpus));
    struct cpu_local *c = &cpus[0];
    c->self            = c;
    c->cpu_id          = 0;
    c->lapic_id        = bsp_lapic_id;
    c->kernel_rsp_top  = 0;
    c->user_rsp_save   = 0;
    c->current         = 0;
    c->idle_task       = 0;
    c->tss             = 0;
    c->online          = 1;
    set_gs_base(c);
    log_write_hex("PERCPU: bsp gs_base   =", (uint64_t)c, KERNEL, LOG_INFO);
    log_write_hex("PERCPU: bsp lapic_id  =", bsp_lapic_id, KERNEL, LOG_INFO);
}

void percpu_init_ap(int cpu_id, uint8_t lapic_id) {
    /* Called from the BSP while staging AP boot. Fills the AP's cpu_local
     * slot but DOES NOT write GS_BASE — that MSR is per-CPU and must be
     * armed by the AP itself once it lands in long mode. */
    if (cpu_id <= 0 || cpu_id >= MAX_CPUS) return;
    struct cpu_local *c = &cpus[cpu_id];
    memset(c, 0, sizeof(*c));
    c->self     = c;
    c->cpu_id   = cpu_id;
    c->lapic_id = lapic_id;
    c->online   = 0;
}

void percpu_arm_gs_this(int cpu_id) {
    /* Called from the AP itself, very early in ap_main, BEFORE anything
     * uses gs-relative addressing. */
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return;
    set_gs_base(&cpus[cpu_id]);
}

struct cpu_local *percpu_get(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return 0;
    return &cpus[cpu_id];
}

struct cpu_local *percpu_this(void) {
    struct cpu_local *c;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(c));
    return c;
}

int  percpu_cpu_count(void)      { return cpu_count; }
void percpu_set_count(int n)     { if (n >= 1 && n <= MAX_CPUS) cpu_count = n; }
