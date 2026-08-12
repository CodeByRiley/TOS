/* kernel/interrupts/pic.c — 8259A PIC driver.
 *
 * Remaps legacy IRQs 0..15 to CPU vectors 0x20..0x2F so they don't
 * collide with the architectural exception range (0..31). Once the
 * LAPIC takes over for routing we keep the PICs around just for
 * spurious-IRQ EOI bookkeeping.
 */
#include "interrupts/pic.h"
#include "devices/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* OCW2, non-specific End Of Interrupt (bits 7-5 = 001). */
#define PIC_EOI   0x20

/* ICW1 — written to the command port, starts the init sequence */
//
// Bits | Name | Description
// 7-5  | —    | Must be 0 in 8086 mode
// 4    | INIT | 1 = Begin initialisation; the next writes are ICW2..ICW4
// 3    | LTIM | 0 = Edge triggered, 1 = Level triggered
// 2    | ADI  | Ignored in 8086 mode
// 1    | SNGL | 0 = Cascaded (an ICW3 follows), 1 = Single PIC
// 0    | IC4  | 1 = An ICW4 follows
#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01

/* ICW2 — vector base. The PIC ORs the IRQ number into the low 3 bits, so
 * the base must be 8-aligned. 0x20/0x28 move IRQ0-15 to vectors 32-47,
 * clear of the architectural exception range 0-31. */
#define ICW2_PIC1_VECTOR 0x20
#define ICW2_PIC2_VECTOR 0x28

/* ICW3 — cascade wiring. Asymmetric by design: the master takes a bitmask
 * of which IRQ lines have a slave attached, the slave takes the line number
 * it is attached to. Both therefore describe the same wire, IRQ2. */
#define ICW3_MASTER_SLAVE_MASK 0x04  /* bit 2 set = slave on IRQ2 */
#define ICW3_SLAVE_ID          0x02  /* this slave is cascaded to IRQ2 */

/* ICW4 */
//
// Bits | Name  | Description
// 7-5  | —     | Must be 0
// 4    | SFNM  | 1 = Special fully nested mode
// 3    | BUF   | 1 = Buffered mode
// 2    | M/S   | Buffered master (1) or slave (0)
// 1    | AEOI  | 1 = Auto EOI; 0 means software must send an EOI
// 0    | uPM   | 1 = 8086/88 mode, 0 = MCS-80/85
#define ICW4_8086  0x01

/* Mask every line. Drivers unmask what they own via pic_clear_mask. */
#define PIC_MASK_ALL 0xFF

void pic_remap(void) {
    /* Order is fixed by the chip: ICW1 to the command port, then ICW2..ICW4
     * to the data port. The PIC counts these writes, so none may be skipped
     * or reordered. */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4);
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4);
    outb(PIC1_DATA, ICW2_PIC1_VECTOR);
    outb(PIC2_DATA, ICW2_PIC2_VECTOR);
    outb(PIC1_DATA, ICW3_MASTER_SLAVE_MASK);
    outb(PIC2_DATA, ICW3_SLAVE_ID);
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, PIC_MASK_ALL);
    outb(PIC2_DATA, PIC_MASK_ALL);
}

void pic_send_eoi(uint8_t irq) {
    /* A slave IRQ reaches the CPU through the master, so both need the EOI —
     * slave first, or the master unblocks before the slave has cleared. */
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}
