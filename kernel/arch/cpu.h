#ifndef CPU_H
#define CPU_H

#include "utilities/types.h"
#include <stdint.h>

SINLINE uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

SINLINE void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

#endif // CPU_H
