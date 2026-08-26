/* kernel/interrupts/pic.h , 8259 PIC surface.
 *
 * The PIC is remapped on boot so legacy IRQs 0..15 land at vectors
 * 0x20..0x2F (avoiding the CPU exception range 0..31). Once the LAPIC
 * is brought up, we keep the PIC masked but still need pic_send_eoi for
 * spurious-IRQ handling.
 *
 * Implementation: kernel/interrupts/pic.c.
 */
#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/* Initialise + remap to vectors 0x20-0x2F. */
void pic_remap(void);

/* End-of-interrupt for IRQ `irq`. */
void pic_send_eoi(uint8_t irq);

/* Per-IRQ mask control. */
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif
