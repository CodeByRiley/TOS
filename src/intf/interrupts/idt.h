/* src/intf/interrupts/idt.h — IDT setup + IRQ install.
 *
 * Long-mode IDT gate descriptors are 16 bytes; the IDTR loaded by lidt
 * is 10 bytes (limit + base). Both layouts are checked at compile time
 * with static_asserts.
 *
 * The kernel exposes an exception_recovery_* surface: setjmp-style
 * recovery for code that may touch a faulting page (the
 * framebuffer probe path uses it). exception_recovery_try() returns 0
 * normally; if an exception fires before clear(), the handler longjmps
 * back and the try() call appears to return non-zero.
 *
 * Implementation: src/impl/kernel/interrupts/idt.c.
 */
#ifndef IDT_H
#define IDT_H

#include <stddef.h>
#include <stdint.h>

/* Long-mode 64-bit interrupt gate. */
struct idt_entry {
    uint16_t offset_low;    /* bits 0-15  of handler                       */
    uint16_t selector;      /* GDT code segment (0x08 from boot GDT)       */
    uint8_t  ist;           /* 0 = use current rsp                         */
    uint8_t  type_attr;     /* 0x8E = present, DPL=0, 64-bit interrupt     */
    uint16_t offset_mid;    /* bits 16-31                                  */
    uint32_t offset_high;   /* bits 32-63                                  */
    uint32_t zero;
} __attribute__((packed));

/* lidt operand. */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

_Static_assert(sizeof(struct idt_entry) == 16,
               "IDT gate descriptor must be 16 bytes in long mode");
_Static_assert(sizeof(struct idt_ptr) == 10,
               "IDTR operand must be 10 bytes in long mode");
_Static_assert(offsetof(struct idt_entry, offset_high) == 8,
               "IDT high offset must start at byte 8");

void idt_init(void);

/* AP entry: load the shared IDTR. */
void idt_load_this_cpu(void);

/* Register a handler for IRQ `irq` (after PIC remap, IRQs are vectors
 * 0x20+). */
void irq_install(uint8_t irq, void (*fn)(void));

/* setjmp-style exception trap. Returns 0 on first call; if any exception
 * fires before exception_recovery_clear(), the handler longjmps back and
 * this returns non-zero. */
int      exception_recovery_try(void);
void     exception_recovery_clear(void);

/* Post-mortem getters valid after a faulted recovery. */
int      exception_recovery_faulted(void);
uint64_t exception_recovery_int_num(void);
uint64_t exception_recovery_err_code(void);
uint64_t exception_recovery_rip(void);

#endif
