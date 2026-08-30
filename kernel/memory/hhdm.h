/* kernel/memory/hhdm.h -- higher-half direct-map conversions. */
#ifndef MEMORY_HHDM_H
#define MEMORY_HHDM_H

#include <utilities/types.h>
#include <stdint.h>

#define HHDM_BASE 0xffff800000000000ULL

#ifdef HHDM_HOST_TEST
/* Host tests model physical memory with an ordinary byte array. Keeping the
 * conversion behind these hooks lets them execute production page-table code
 * without trying to dereference the kernel's canonical HHDM addresses. */
void *hhdm_test_phys_to_virt(u64 phys);
u64 hhdm_test_virt_to_phys(const void *virt);

SINLINE void *phys_to_virt(u64 phys) {
    return hhdm_test_phys_to_virt(phys);
}

SINLINE u64 virt_to_phys(const void *virt) {
    return hhdm_test_virt_to_phys(virt);
}
#else
/* These convert physical addresses and pointers within the direct map only.
 * Do not use virt_to_phys() on ordinary kernel-image or heap pointers. */
SINLINE void *phys_to_virt(u64 phys) {
    return (void *)(uintptr_t)(HHDM_BASE + phys);
}

SINLINE u64 virt_to_phys(const void *virt) {
    return (u64)(uintptr_t)virt - HHDM_BASE;
}
#endif

#endif
