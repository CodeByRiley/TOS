/* kernel/sched/smp.c , SMP boot orchestration.
 *
 * The BSP walks the ACPI cpu list, prepares per-CPU state (kstack, TSS
 * descriptor, percpu slot), copies the AP real-mode trampoline to
 * physical 0x8000, and drives INIT-SIPI-SIPI for each AP. Each AP runs
 * the trampoline (real → protected → long mode), reaches ap_main, and
 * services the interrupt-driven SMP-safe kernel work queue.
 *
 * The trampoline source lives in kernel/arch/x86_64/boot/ap_trampoline.asm.
 */
#include <sched/smp.h>
#include <acpi/acpi.h>
#include <arch/gdt.h>
#include <arch/percpu.h>
#include <arch/syscall.h>
#include <devices/lapic.h>
#include <devices/pit.h>
#include <interrupts/idt.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <arch/cpu.h>
#include <utilities/log.h>
#include <utilities/string.h>
#include <sync/spinlock.h>
#include <stdint.h>

/* AP bring-up. Copies the 16-bit trampoline to physical address 0x8000,
 * patches in this CPU's stack + PML4 + C entry + cpu_id, then drives the
 * Intel-mandated INIT-SIPI-SIPI sequence via the LAPIC. Waits for each AP
 * to set cpu_local.online before moving on to the next one. */

/* Emitted by objcopy from ap_trampoline.bin. The Makefile runs objcopy from
 * the blob's directory so these names stay path-independent. */
extern u8 _binary_ap_trampoline_bin_start[];
extern u8 _binary_ap_trampoline_bin_end[];

extern u8 page_table_l4[];
extern void ap_long_mode_handoff(void);

#define AP_TRAMPOLINE_PHYS  0x8000
#define AP_KSTACK_BYTES     16384
#define SMP_WORK_VECTOR     240
#define SMP_WORK_CAPACITY   64

struct smp_work_item {
    smp_work_fn fn;
    void *arg;
};

static struct smp_work_item work_queue[SMP_WORK_CAPACITY];
static u32 work_head;
static u32 work_tail;
static struct spinlock work_lock = SPINLOCK_INIT;
static volatile int online_workers;
static volatile u64 completed_jobs;

static int work_pop(struct smp_work_item *out) {
    int found = 0;
    spin_lock(&work_lock);
    if (work_head != work_tail) {
        *out = work_queue[work_tail];
        work_tail = (work_tail + 1) % SMP_WORK_CAPACITY;
        found = 1;
    }
    spin_unlock(&work_lock);
    return found;
}

static void kick_workers(void) {
    int count = percpu_cpu_count();
    for (int i = 1; i < count; i++) {
        struct cpu_local *cpu = percpu_get(i);
        if (cpu && __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE))
            lapic_send_fixed(cpu->lapic_id, SMP_WORK_VECTOR);
    }
}

int smp_submit_work(smp_work_fn fn, void *arg) {
    if (!fn || __atomic_load_n(&online_workers, __ATOMIC_ACQUIRE) == 0)
        return -1;

    spin_lock(&work_lock);
    u32 next = (work_head + 1) % SMP_WORK_CAPACITY;
    if (next == work_tail) {
        spin_unlock(&work_lock);
        return -1;
    }
    work_queue[work_head].fn = fn;
    work_queue[work_head].arg = arg;
    work_head = next;
    spin_unlock(&work_lock);

    kick_workers();
    return 0;
}

int smp_worker_count(void) {
    return __atomic_load_n(&online_workers, __ATOMIC_ACQUIRE);
}

u64 smp_completed_work(void) {
    return __atomic_load_n(&completed_jobs, __ATOMIC_ACQUIRE);
}

struct smp_probe {
    volatile u32 completed;
    volatile u32 cpu_mask;
};

static void smp_probe_job(void *arg) {
    struct smp_probe *probe = (struct smp_probe *)arg;
    struct cpu_local *cpu = percpu_this();
    if (cpu && cpu->cpu_id < 32)
        __atomic_fetch_or(&probe->cpu_mask, 1u << cpu->cpu_id,
                          __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&probe->completed, 1, __ATOMIC_ACQ_REL);
}

/* Trailer layout produced by ap_trampoline.asm (offsets from end-of-binary):
 *   -32: ap_pml4_phys   (u32)
 *   -28: ap_cpu_id      (u32)
 *   -24: ap_stack_top   (u64)
 *   -16: ap_handoff     (u64)
 *    -8: ap_target_cr3  (u64)
 */
#define AP_PATCH_PML4_OFF    32
#define AP_PATCH_CPUID_OFF   28
#define AP_PATCH_STACK_OFF   24
#define AP_PATCH_HANDOFF_OFF 16
#define AP_PATCH_TARGET_OFF   8

/* Spin for roughly `us` microseconds using PIT ticks (100 Hz = 10 ms each).
 * Granularity is 10 ms , we round up to at least one tick. Good enough for
 * the Intel-spec 10 ms inter-INIT delay and the 200 µs inter-SIPI delay
 * (which we just round up to one 10 ms PIT tick , well within spec). */
static void smp_delay_us(u64 us) {
    u64 ticks = (us + 9999) / 10000;
    if (ticks == 0) ticks = 1;
    u64 start = pit_ticks();
    while (pit_ticks() - start < ticks) {
        __asm__ volatile ("pause");
    }
}

static int boot_one_ap(int cpu_id, u8 apic_id, u32 bootstrap_cr3,
                       u64 target_cr3) {
    void *stack = kmalloc(AP_KSTACK_BYTES);
    if (!stack) {
        log_write("SMP: AP kstack alloc failed", KERNEL, LOG_ERROR);
        return -1;
    }
    u64 stack_top = ((u64)stack + AP_KSTACK_BYTES) & ~0xFULL;
    u64 entry_stack = stack_top - 8;

    percpu_init_ap(cpu_id, apic_id);
    percpu_get(cpu_id)->kernel_rsp_top = stack_top;
    gdt_install_tss(cpu_id, stack_top);

    u8 *t   = phys_to_virt(AP_TRAMPOLINE_PHYS);
    usize   len = (usize)(_binary_ap_trampoline_bin_end -
                            _binary_ap_trampoline_bin_start);
    memcpy(t, _binary_ap_trampoline_bin_start, len);
    *(u32*)(t + len - AP_PATCH_PML4_OFF)  = bootstrap_cr3;
    *(u32*)(t + len - AP_PATCH_CPUID_OFF) = (u32)cpu_id;
    *(u64*)(t + len - AP_PATCH_STACK_OFF) = entry_stack;
    *(u64*)(t + len - AP_PATCH_HANDOFF_OFF) =
        (u64)ap_long_mode_handoff;
    *(u64*)(t + len - AP_PATCH_TARGET_OFF) = target_cr3;

    /* INIT-SIPI-SIPI sequence per Intel SDM Vol 3A §8.4.4.1. */
    lapic_send_init(apic_id);
    smp_delay_us(10000);                  /* 10 ms */
    lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);
    smp_delay_us(200);
    lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);

    /* Spin for up to ~1 s waiting for the AP to flag itself online. */
    struct cpu_local *c = percpu_get(cpu_id);
    for (int i = 0; i < pit_get_freq() && !c->online; i++) {
        smp_delay_us(10000);
    }
    if (!c->online) {
        log_write_hex("SMP: AP failed to come online cpu_id=", cpu_id, KERNEL, LOG_ERROR);
        return -1;
    }
    return 0;
}

void smp_boot_aps(void) {
    int n = acpi_cpu_count();
    if (n <= 1) {
        log_write("SMP: only 1 CPU, no APs to start", KERNEL, LOG_INFO);
        return;
    }
    if (n > MAX_CPUS) n = MAX_CPUS;

    u64 bootstrap_phys = (u64)(uintptr_t)page_table_l4;
    if ((bootstrap_phys & 0xFFFULL) || bootstrap_phys > 0xFFFFFFFFULL) {
        log_write("SMP: bootstrap PML4 is not a low aligned frame", KERNEL,
                  LOG_ERROR);
        return;
    }

    /* Give APs a distinct root while sharing all lower-level kernel tables.
     * This exercises the two-stage handoff now, and the frame is allowed to
     * land above 4 GiB because only the 64-bit handoff loads it into CR3. */
    u64 source_cr3 = read_cr3() & ~0xFFFULL;
    u64 target_cr3 = pmm_alloc_frame();
    if (!target_cr3) {
        log_write("SMP: AP PML4 allocation failed", KERNEL, LOG_ERROR);
        return;
    }
    memcpy(phys_to_virt(target_cr3), phys_to_virt(source_cr3), 4096);

    log_write_hex("SMP: bootstrap CR3    =", bootstrap_phys, KERNEL,
                  LOG_INFO);
    log_write_hex("SMP: AP target CR3    =", target_cr3, KERNEL, LOG_INFO);

    /* CPU 0 is the BSP , already running. Bring up 1..n-1. */
    for (int i = 1; i < n; i++) {
        boot_one_ap(i, acpi_cpu_apic_id(i), (u32)bootstrap_phys,
                    target_cr3);
    }

    int workers = smp_worker_count();
    if (workers <= 0)
        return;

    static struct smp_probe probe;
    probe.completed = 0;
    probe.cpu_mask = 0;
    int jobs = workers * 4;
    for (int i = 0; i < jobs; i++) {
        if (smp_submit_work(smp_probe_job, &probe) != 0) {
            jobs = i;
            break;
        }
    }

    u64 start = pit_ticks();
    while ((int)__atomic_load_n(&probe.completed, __ATOMIC_ACQUIRE) < jobs &&
           pit_ticks() - start < 100) {
        __asm__ volatile ("pause");
    }
    log_write_hex("SMP: AP workers       =", (u64)workers, KERNEL,
                  LOG_INFO);
    log_write_hex("SMP: jobs completed  =", probe.completed, KERNEL, LOG_INFO);
    log_write_hex("SMP: worker cpu mask =", probe.cpu_mask, KERNEL, LOG_INFO);
}

void ap_main(u32 cpu_id) {
    /* We landed here from ap_trampoline.asm. RSP is our own kstack, CR3 is
     * the final AP kernel PML4, and GDTR still points at the trampoline's
     * temporary GDT. The stackless higher-half handoff performed the CR3/RSP
     * transition before entering ordinary C. */

    gdt_load_this_cpu_full();

    /* Segment reload above writes GS, so set GS_BASE only after it. */
    percpu_arm_gs_this((int)cpu_id);
    gdt_load_tss_this_cpu((int)cpu_id);
    idt_load_this_cpu();

    struct cpu_local *me = percpu_get((int)cpu_id);
    syscall_init_this_cpu(me->kernel_rsp_top);

    /* Bring up the local APIC on this CPU (BSP already mapped the MMIO). */
    lapic_enable_this_cpu();

    __atomic_add_fetch(&online_workers, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&me->online, 1, __ATOMIC_RELEASE);

    /* APs execute kernel work only. Userspace remains on the BSP until the
     * scheduler, syscall scratch state and drivers are all made per-CPU. */
    for (;;) {
        struct smp_work_item item;
        __asm__ volatile ("cli" ::: "memory");
        if (work_pop(&item)) {
            __asm__ volatile ("sti" ::: "memory");
            item.fn(item.arg);
            __atomic_add_fetch(&completed_jobs, 1, __ATOMIC_ACQ_REL);
            continue;
        }

        /* STI delays interrupt recognition until after HLT. A job queued
         * between the empty check and this pair leaves an IPI pending, which
         * wakes the CPU instead of losing the notification. */
        __asm__ volatile ("sti; hlt" ::: "memory");
    }
}
