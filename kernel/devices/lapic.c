/* kernel/devices/lapic.c , Local APIC MMIO driver.
 *
 * Maps the LAPIC's 4 KiB MMIO window at a fixed kernel VA. The same
 * physical base is shared by every CPU , each core's LAPIC just appears
 * at this address when CR3 has the kernel PML4 linked in. Reads/writes
 * are 32-bit memory-mapped ops; the page is mapped uncacheable so they
 * don't get reordered or coalesced.
 */
#include "utilities/panic.h"
#include <devices/lapic.h>
#include <memory/vmm.h>
#include <stdint.h>
#include <utilities/log.h>

#define LAPIC_VIRT_BASE 0xFFFFE00100000000ULL

static volatile u8 *lapic_mmio = 0;

void lapic_init(u64 mmio_phys) {
  if (lapic_mmio)
    return;
  /* Map a single 4 KiB page. Mark uncacheable via PCD/PWT so MMIO loads
   * and stores don't get reordered or coalesced by the cache. */
  vmm_map(LAPIC_VIRT_BASE, mmio_phys,
          VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT);
  lapic_mmio = (volatile u8 *)LAPIC_VIRT_BASE;
  lapic_enable_this_cpu();
  log_write_hex("LAPIC: mapped phys =", mmio_phys, KERNEL, LOG_INFO);
  log_write_hex("LAPIC: id          =", lapic_id(), KERNEL, LOG_INFO);
}

void lapic_enable_this_cpu(void) {
  /* Software-enable the LAPIC: set bit 8 of the SVR and route spurious
   * interrupts to vector 0xFF (a no-op IDT slot we mask in the handler). */
  lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | 0xFF);
  lapic_write(LAPIC_REG_TPR, 0); /* accept all interrupt priorities */
}

u32 lapic_read(u32 reg) { return *(volatile u32 *)(lapic_mmio + reg); }

void lapic_write(u32 reg, u32 val) {
  *(volatile u32 *)(lapic_mmio + reg) = val;
}

u32 lapic_id(void) { return lapic_read(LAPIC_REG_ID) >> 24; }

void lapic_eoi(void) { lapic_write(LAPIC_REG_EOI, 0); }

static void icr_wait(void) {
  int timeout = 1000000; // Arbitrary large number
  /* Poll the delivery-status bit until the previous IPI is acknowledged. */
  while ((lapic_read(LAPIC_REG_ICR_LOW) & LAPIC_ICR_PENDING) && --timeout) {
    __asm__ volatile("pause");
  }
  if (timeout == 0) {
    panic("lapic: ICR delivery timed out");
  }
}

void lapic_send_init(u8 apic_id) {
  lapic_write(LAPIC_REG_ICR_HIGH, (u32)apic_id << 24);
  lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
                                     LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL_LEVEL);
  icr_wait();
}

void lapic_send_startup(u8 apic_id, u8 vector) {
  lapic_write(LAPIC_REG_ICR_HIGH, (u32)apic_id << 24);
  lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_STARTUP | LAPIC_ICR_PHYSICAL |
                                     LAPIC_ICR_ASSERT | (u32)vector);
  icr_wait();
}

void lapic_send_fixed(u8 apic_id, u8 vector) {
  lapic_write(LAPIC_REG_ICR_HIGH, (u32)apic_id << 24);
  lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                                     LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL_EDGE |
                                     vector);
  icr_wait();
}
