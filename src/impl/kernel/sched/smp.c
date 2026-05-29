#include "sched/smp.h"
#include "acpi/acpi.h"
#include "arch/gdt.h"
#include "arch/percpu.h"
#include "devices/lapic.h"
#include "devices/pit.h"
#include "interrupts/idt.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

/* AP bring-up. Copies the 16-bit trampoline to physical address 0x8000,
 * patches in this CPU's stack + PML4 + C entry + cpu_id, then drives the
 * Intel-mandated INIT-SIPI-SIPI sequence via the LAPIC. Waits for each AP
 * to set cpu_local.online before moving on to the next one. */

extern uint8_t _binary_build_x86_64_boot_ap_trampoline_bin_start[];
extern uint8_t _binary_build_x86_64_boot_ap_trampoline_bin_end[];

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

static uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

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
    log_write_hex("SMP: boot_one_ap cpu_id=", cpu_id, KERNEL, LOG_INFO);
    log_write_hex("SMP:   apic_id        =", apic_id, KERNEL, LOG_INFO);
    void *stack = kmalloc(AP_KSTACK_BYTES);
    if (!stack) {
        log_write("SMP: AP kstack alloc failed", KERNEL, LOG_ERROR);
        return -1;
    }
    uint64_t stack_top = (uint64_t)stack + AP_KSTACK_BYTES;
    log_write_hex("SMP:   kstack_top     =", stack_top, KERNEL, LOG_INFO);

    percpu_init_ap(cpu_id, apic_id);
    gdt_install_tss(cpu_id, stack_top);
    log_write("SMP:   tss installed", KERNEL, LOG_INFO);

    uint8_t *t   = (uint8_t*)AP_TRAMPOLINE_PHYS;
    size_t   len = (size_t)(_binary_build_x86_64_boot_ap_trampoline_bin_end -
                            _binary_build_x86_64_boot_ap_trampoline_bin_start);
    log_write_hex("SMP:   trampoline len =", len, KERNEL, LOG_INFO);
    memcpy(t, _binary_build_x86_64_boot_ap_trampoline_bin_start, len);
    *(uint32_t*)(t + len - AP_PATCH_PML4_OFF)  = pml4_phys;
    *(uint32_t*)(t + len - AP_PATCH_CPUID_OFF) = (uint32_t)cpu_id;
    *(uint64_t*)(t + len - AP_PATCH_STACK_OFF) = stack_top;
    *(uint64_t*)(t + len - AP_PATCH_ENTRY_OFF) = (uint64_t)ap_main;
    log_write("SMP:   trampoline copied + patched", KERNEL, LOG_INFO);

    /* INIT-SIPI-SIPI sequence per Intel SDM Vol 3A §8.4.4.1. */
    lapic_send_init(apic_id);
    log_write("SMP:   INIT sent", KERNEL, LOG_INFO);
    smp_delay_us(10000);                  /* 10 ms */
    lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);
    log_write("SMP:   SIPI #1 sent", KERNEL, LOG_INFO);
    smp_delay_us(200);
    lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);
    log_write("SMP:   SIPI #2 sent - waiting for online", KERNEL, LOG_INFO);

    /* Spin for up to ~1 s waiting for the AP to flag itself online. */
    struct cpu_local *c = percpu_get(cpu_id);
    for (int i = 0; i < 100 && !c->online; i++) {
        smp_delay_us(10000);
    }
    if (!c->online) {
        log_write_hex("SMP: AP failed to come online cpu_id=", cpu_id, KERNEL, LOG_ERROR);
        return -1;
    }
    log_write_hex("SMP: AP online cpu_id=", cpu_id, KERNEL, LOG_INFO);
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

    /* Identity-mapping for 0x8000 already exists from boot's 1 GiB 2 MiB
     * huge-page tables — no extra vmm work needed before the memcpy. */
    log_write_hex("SMP: trampoline phys =", AP_TRAMPOLINE_PHYS, KERNEL, LOG_INFO);

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
