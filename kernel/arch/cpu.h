#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <utilities/types.h>

#if !defined(__x86_64__) && !defined(__i386__)
#error "cpu.h requires an x86 target"
#endif

SINLINE u64 read_cr3(void)
{
    u64 value;

    __asm__ volatile (
        "mov %%cr3, %0"
        : "=r"(value)
        :
        : "memory"
    );

    return value;
}

SINLINE void write_cr3(u64 value)
{
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(value)
        : "memory"
    );
}

#endif /* CPU_H */
