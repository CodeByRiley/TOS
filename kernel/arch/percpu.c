/* kernel/arch/percpu.c — per-CPU data.
 *
 * One struct cpu_local per logical CPU. GS_BASE on each core points at
 * its own slot. BSP populates its slot during early kernel_main; each
 * AP populates its slot from inside ap_main() BEFORE touching anything
 * that uses gs-relative addressing.
 *
 * GS_BASE + KERNEL_GS_BASE both get the same pointer. We don't actually
 * use SWAPGS (userspace doesn't touch GS) but keeping both MSRs in sync
 * means a stray SWAPGS — including one added later — won't leave GS
 * pointing at oblivion.
 */
#include <arch/percpu.h>
#include <utilities/log.h>
#include <utilities/string.h>
#include <stdint.h>

#define MSR_GS_BASE         0xC0000101u
#define MSR_KERNEL_GS_BASE  0xC0000102u

static struct cpu_local cpus[MAX_CPUS] __attribute__((aligned(64)));
static int              cpu_count = 1;

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
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

int percpu_current_id(void) {
    uint64_t base = rdmsr(MSR_GS_BASE);
    for (int i = 0; i < MAX_CPUS; i++) {
        if (base == (uint64_t)(uintptr_t)&cpus[i])
            return i;
    }
    return 0;
}

int percpu_cpu_count(void) { return cpu_count; }

void percpu_set_count(int n) {
    if (n < 1) n = 1;
    if (n > MAX_CPUS) n = MAX_CPUS;
    cpu_count = n;
}
