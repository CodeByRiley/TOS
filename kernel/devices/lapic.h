/* kernel/devices/lapic.h — Local APIC MMIO driver.
 *
 * Maps the 4 KiB MMIO window into kernel VA space and exposes the subset
 * of registers we actually touch: EOI (acknowledge), ICR (send IPIs),
 * SVR (software enable), and the LVT timer (per-CPU preemption).
 *
 * Implementation: kernel/devices/lapic.c.
 */
#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

/* --- LAPIC register offsets ----------------------------------------- */
#define LAPIC_REG_ID         0x020
#define LAPIC_REG_VERSION    0x030
#define LAPIC_REG_TPR        0x080
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_LDR        0x0D0
#define LAPIC_REG_DFR        0x0E0
#define LAPIC_REG_SVR        0x0F0
#define LAPIC_REG_ESR        0x280
#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310
#define LAPIC_REG_LVT_TIMER  0x320
#define LAPIC_REG_LVT_LINT0  0x350
#define LAPIC_REG_LVT_LINT1  0x360
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CURR 0x390
#define LAPIC_REG_TIMER_DIV  0x3E0

#define LAPIC_SVR_ENABLE     0x100      /* software enable bit */

/* --- ICR delivery modes + flags ------------------------------------- */
#define LAPIC_ICR_FIXED       (0u << 8)
#define LAPIC_ICR_INIT        (5u << 8)
#define LAPIC_ICR_STARTUP     (6u << 8)
#define LAPIC_ICR_PHYSICAL    (0u << 11)
#define LAPIC_ICR_ASSERT      (1u << 14)
#define LAPIC_ICR_DEASSERT    (0u << 14)
#define LAPIC_ICR_LEVEL_EDGE  (0u << 15)
#define LAPIC_ICR_LEVEL_LEVEL (1u << 15)
#define LAPIC_ICR_PENDING     (1u << 12)

/* BSP entry: map MMIO and enable for this CPU. */
void     lapic_init(uint64_t mmio_phys);

/* AP entry: just enable — BSP already mapped the MMIO page. */
void     lapic_enable_this_cpu(void);

uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t val);
uint32_t lapic_id(void);
void     lapic_eoi(void);

/* INIT-SIPI-SIPI sequence for AP startup. `vector` is the high byte of
 * the AP's real-mode start address: vec=0x08 → AP starts at 0x8000.
 * Caller handles inter-IPI delays (10 ms after INIT, 200 us after each
 * SIPI per Intel SDM). */
void lapic_send_init(uint8_t apic_id);
void lapic_send_startup(uint8_t apic_id, uint8_t vector);

#endif
