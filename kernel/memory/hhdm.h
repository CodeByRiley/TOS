/* kernel/memory/hhdm.h -- higher-half direct-map conversions. */
#ifndef MEMORY_HHDM_H
#define MEMORY_HHDM_H

#include <stdint.h>

#define HHDM_BASE 0xffff800000000000ULL

/* These convert physical addresses and pointers within the direct map only.
 * Do not use virt_to_phys() on ordinary kernel-image or heap pointers. */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(HHDM_BASE + phys);
}

static inline uint64_t virt_to_phys(const void *virt) {
    return (uint64_t)(uintptr_t)virt - HHDM_BASE;
}

#endif
