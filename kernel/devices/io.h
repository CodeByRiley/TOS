/* kernel/devices/io.h , port I/O primitives.
 *
 * inb/outb + 16/32-bit variants plus an `io_wait()` idiom. All static
 * inlines so call sites get folded into the right asm instructions and
 * no .c file is needed.
 */
#ifndef IO_H
#define IO_H

#include <utilities/types.h>
#include <stdint.h>

SINLINE void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

SINLINE u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 16-bit variants. */
SINLINE void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

SINLINE u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 32-bit variants (used by PCI config-space access). */
SINLINE void outl(u16 port, u32 val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

SINLINE u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Read RFLAGS and test the Interrupt Flag (bit 9). Anything that waits on the
 * PIT tick counter, blocks, or yields needs this to be true: with IF clear,
 * IRQ0 never fires, the tick never advances, and the wait never ends. */
SINLINE int interrupts_enabled(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

/* "Wait one ISA bus cycle" , write to BIOS POST diagnostic port 0x80,
 * which is unused on modern systems and has no side effects. Legacy
 * idiom for spacing out back-to-back PIC operations etc. */
SINLINE void io_wait(void) {
    outb(0x80, 0);
}

#endif
