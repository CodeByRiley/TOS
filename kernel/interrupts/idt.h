/* kernel/interrupts/idt.h , IDT setup + IRQ install.
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
 * Implementation: kernel/interrupts/idt.c.
 */
#ifndef IDT_H
#define IDT_H


/* PACKED and friends. */
#include <utilities/types.h>
#include <stddef.h>
#include <stdint.h>

/* Long-mode 64-bit interrupt gate (16 bytes).
 *
 * type_attr layout:
 *   Bits | Name    | Description
 *   7    | Present | 0 = Unhandled vector, faults as #NP
 *   6-5  | DPL     | Lowest ring that may invoke this by `int n`
 *   4    | Zero    | Must be 0 for a system descriptor
 *   3-0  | Type    | 0xE = interrupt gate (clears IF), 0xF = trap gate (leaves IF)
 *
 * So 0x8E is present, DPL 0, interrupt gate. Use 0xEE (DPL 3) only for a
 * vector userspace is meant to raise deliberately. */
struct idt_entry {
    u16 offset_low;    /* bits 0-15  of handler                       */
    u16 selector;      /* GDT code segment (0x08 from boot GDT)       */
    u8  ist;           /* 0 = use current rsp, 1-7 = TSS IST stack    */
    u8  type_attr;
    u16 offset_mid;    /* bits 16-31                                  */
    u32 offset_high;   /* bits 32-63                                  */
    u32 zero;
} PACKED;

/* lidt operand. */
struct idt_ptr {
    u16 limit;
    u64 base;
} PACKED;

_Static_assert(sizeof(struct idt_entry) == 16,
               "IDT gate descriptor must be 16 bytes in long mode");
_Static_assert(sizeof(struct idt_ptr) == 10,
               "IDTR operand must be 10 bytes in long mode");
_Static_assert(offsetof(struct idt_entry, offset_high) == 8,
               "IDT high offset must start at byte 8");

/* Register image built by isr_common in isr.asm. SS:RSP are present only
 * when the interrupt crossed privilege levels; callers must check CS.RPL
 * before reading them. */
struct interrupt_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 int_num;
    u64 err_code;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

_Static_assert(offsetof(struct interrupt_frame, int_num) == 120,
               "ISR vector offset must match isr.asm");
_Static_assert(offsetof(struct interrupt_frame, rip) == 136,
               "ISR RIP offset must match the CPU exception frame");
_Static_assert(offsetof(struct interrupt_frame, cs) == 144,
               "ISR CS offset must match the swapgs privilege check");

void idt_init(void);

/* AP entry: load the shared IDTR. */
void idt_load_this_cpu(void);

/* Register a handler for IRQ `irq` (after PIC remap, IRQs are vectors
 * 0x20+). */
void irq_install(u8 irq, void (*fn)(void));

/* setjmp-style exception trap. Returns 0 on first call; if any exception
 * fires before exception_recovery_clear(), the handler longjmps back and
 * this returns non-zero. */
int      exception_recovery_try(void);
void     exception_recovery_clear(void);

/* Post-mortem getters valid after a faulted recovery. */
int      exception_recovery_faulted(void);
u64 exception_recovery_int_num(void);
u64 exception_recovery_err_code(void);
u64 exception_recovery_rip(void);

#endif
