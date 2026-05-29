#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;    // bits 0-15  of handler
    uint16_t selector;      // GDT code segment (0x08 from boot GDT)
    uint8_t  ist;           // 0 = use current rsp
    uint8_t  type_attr;     // 0x8E = present, DPL=0, 64-bit interrupt gate
    uint16_t offset_mid;    // bits 16-31
    uint32_t offset_high;   // bits 32-63
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_init(void);
void idt_load_this_cpu(void);   /* AP entry: load the (shared) IDTR */
void irq_install(uint8_t irq, void (*fn)(void));
int exception_recovery_try(void);
void exception_recovery_clear(void);
int exception_recovery_faulted(void);
uint64_t exception_recovery_int_num(void);
uint64_t exception_recovery_err_code(void);
uint64_t exception_recovery_rip(void);

#endif
