/* kernel/sched/smp.c — SMP boot orchestration.
 *
 * The BSP walks the ACPI cpu list, prepares per-CPU state (kstack, TSS
 * descriptor, percpu slot), copies the AP real-mode trampoline to
 * physical 0x8000, and drives INIT-SIPI-SIPI for each AP. Each AP runs
 * the trampoline (real → protected → long mode), reaches ap_main, and
 * spins until it gets queued onto the per-CPU idle task.
 *
 * The trampoline source lives in kernel/arch/x86_64/boot/ap_trampoline.asm.
 */
#include "sched/smp.h"
#include "acpi/acpi.h"
#include "arch/gdt.h"
#include "arch/percpu.h"
#include "devices/lapic.h"
#include "devices/pit.h"
#include "interrupts/idt.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "memory/vmm.h"
#include "arch/cpu.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

/* AP bring-up. Copies the 16-bit trampoline to physical address 0x8000,
 * patches in this CPU's stack + PML4 + C entry + cpu_id, then drives the
 * Intel-mandated INIT-SIPI-SIPI sequence via the LAPIC. Waits for each AP
 * to set cpu_local.online before moving on to the next one. */

/* Emitted by objcopy from ap_trampoline.bin. The Makefile runs objcopy from
 * the blob's directory so these names stay path-independent. */
extern uint8_t _binary_ap_trampoline_bin_start[];
extern uint8_t _binary_ap_trampoline_bin_end[];

#define AP_TRAMPOLINE_PHYS  0x8000
#define AP_KSTACK_BYTES     16384

/* Trailer layout produced by ap_trampoline.asm (offsets from end-of-binary):
 *   -24: ap_pml4_phys   (uint32_t)
 *   -20: ap_cpu_id      (uint32_t)
 *   -16: ap_stack_top   (uint64_t)
 *    -8: ap_c_entry     (uint64_t)
 */
#define AP_PATCH_PML4_OFF   24
#define AP_PATCH_CPUID_OFF  20
#define AP_PATCH_STACK_OFF  16
#define AP_PATCH_ENTRY_OFF   8

/* Spin for roughly `us` microseconds using PIT ticks (100 Hz = 10 ms each).
 * Granularity is 10 ms — we round up to at least one tick. Good enough for
 * the Intel-spec 10 ms inter-INIT delay and the 200 µs inter-SIPI delay
 * (which we just round up to one 10 ms PIT tick — well within spec). */
static void smp_delay_us(uint64_t us) {
    uint64_t ticks = (us + 9999) / 10000;
    if (ticks == 0) ticks = 1;
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < ticks) {
        __asm__ volatile ("pause");
    }
}

static int boot_one_ap(int cpu_id, uint8_t apic_id, uint32_t pml4_phys) {
    void *stack = kmalloc(AP_KSTACK_BYTES);
    if (!stack) {
        log_write("SMP: AP kstack alloc failed", KERNEL, LOG_ERROR);
        return -1;
    }
    uint64_t stack_top = ((uint64_t)stack + AP_KSTACK_BYTES) & ~0xFULL;
    uint64_t entry_stack = stack_top - 8;

    percpu_init_ap(cpu_id, apic_id);
    gdt_install_tss(cpu_id, stack_top);

    uint8_t *t   = phys_to_virt(AP_TRAMPOLINE_PHYS);
    size_t   len = (size_t)(_binary_ap_trampoline_bin_end -
                            _binary_ap_trampoline_bin_start);
    memcpy(t, _binary_ap_trampoline_bin_start, len);
    *(uint32_t*)(t + len - AP_PATCH_PML4_OFF)  = pml4_phys;
    *(uint32_t*)(t + len - AP_PATCH_CPUID_OFF) = (uint32_t)cpu_id;
    *(uint64_t*)(t + len - AP_PATCH_STACK_OFF) = entry_stack;
    *(uint64_t*)(t + len - AP_PATCH_ENTRY_OFF) = (uint64_t)ap_main;

    /* INIT-SIPI-SIPI sequence per Intel SDM Vol 3A §8.4.4.1. */
    lapic_send_init(apic_id);
    smp_delay_us(10000);                  /* 10 ms */
    lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);
    smp_delay_us(200);
    lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);

    /* Spin for up to ~1 s waiting for the AP to flag itself online. */
    struct cpu_local *c = percpu_get(cpu_id);
    for (int i = 0; i < 100 && !c->online; i++) {
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

    uint64_t cr3 = read_cr3() & ~0xFFFULL;
    if (cr3 > 0xFFFFFFFFULL) {
        log_write("SMP: kernel PML4 above 4 GiB — trampoline can't load it",
                  KERNEL, LOG_ERROR);
        return;
    }
    uint32_t pml4_phys = (uint32_t)cr3;

    /* CPU 0 is the BSP — already running. Bring up 1..n-1. */
    for (int i = 1; i < n; i++) {
        boot_one_ap(i, acpi_cpu_apic_id(i), pml4_phys);
    }
}

void ap_main(uint32_t cpu_id) {
    /* We landed here from ap_trampoline.asm. RSP is our own kstack, CR3 is
     * the kernel PML4, and GDTR still points at the trampoline's tiny GDT. */
    gdt_load_this_cpu_full();

    /* Segment reload above writes GS, so set GS_BASE only after it. */
    percpu_arm_gs_this((int)cpu_id);

    gdt_load_tss_this_cpu((int)cpu_id);
    idt_load_this_cpu();

    /* Bring up the local APIC on this CPU (BSP already mapped the MMIO). */
    lapic_enable_this_cpu();

    struct cpu_local *me = percpu_get((int)cpu_id);
    me->online = 1;

    /* No scheduler integration yet — just halt forever with interrupts
     * masked. Stage 10 will replace this with the AP idle loop that pulls
     * tasks from the shared ready queue under the BKL. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
