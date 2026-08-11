/* kernel/devices/io.h — port I/O primitives.
 *
 * inb/outb + 16/32-bit variants plus an `io_wait()` idiom. All static
 * inlines so call sites get folded into the right asm instructions and
 * no .c file is needed.
 */
#ifndef IO_H
#define IO_H

#include <stdint.h>

/* Write a byte to an I/O port. */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Read a byte from an I/O port. */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 16-bit variants. */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 32-bit variants (used by PCI config-space access). */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* "Wait one ISA bus cycle" — write to BIOS POST diagnostic port 0x80,
 * which is unused on modern systems and has no side effects. Legacy
 * idiom for spacing out back-to-back PIC operations etc. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
