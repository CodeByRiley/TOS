/* kernel/devices/lapic.c — Local APIC MMIO driver.
 *
 * Maps the LAPIC's 4 KiB MMIO window at a fixed kernel VA. The same
 * physical base is shared by every CPU — each core's LAPIC just appears
 * at this address when CR3 has the kernel PML4 linked in. Reads/writes
 * are 32-bit memory-mapped ops; the page is mapped uncacheable so they
 * don't get reordered or coalesced.
 */
#include "devices/lapic.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include <stdint.h>

#define LAPIC_VIRT_BASE 0xFFFFE00100000000ULL

static volatile uint8_t *lapic_mmio = 0;

void lapic_init(uint64_t mmio_phys) {
    if (lapic_mmio) return;
    /* Map a single 4 KiB page. Mark uncacheable via PCD/PWT so MMIO loads
     * and stores don't get reordered or coalesced by the cache. */
    vmm_map(LAPIC_VIRT_BASE, mmio_phys,
            VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT);
    lapic_mmio = (volatile uint8_t*)LAPIC_VIRT_BASE;
    lapic_enable_this_cpu();
    log_write_hex("LAPIC: mapped phys =", mmio_phys, KERNEL, LOG_INFO);
    log_write_hex("LAPIC: id          =", lapic_id(), KERNEL, LOG_INFO);
}

void lapic_enable_this_cpu(void) {
    /* Software-enable the LAPIC: set bit 8 of the SVR and route spurious
     * interrupts to vector 0xFF (a no-op IDT slot we mask in the handler). */
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | 0xFF);
    lapic_write(LAPIC_REG_TPR, 0);       /* accept all interrupt priorities */
}

uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t*)(lapic_mmio + reg);
}

void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(lapic_mmio + reg) = val;
}

uint32_t lapic_id(void) {
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

static void icr_wait(void) {
    /* Poll the delivery-status bit until the previous IPI is acknowledged. */
    while (lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_PENDING) {
        __asm__ volatile ("pause");
    }
}

void lapic_send_init(uint8_t apic_id) {
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL_LEVEL);
    icr_wait();
}

void lapic_send_startup(uint8_t apic_id, uint8_t vector) {
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_STARTUP | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | (uint32_t)vector);
    icr_wait();
}
