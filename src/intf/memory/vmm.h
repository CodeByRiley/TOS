#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define VMM_PRESENT  (1ULL << 0)
#define VMM_WRITE    (1ULL << 1)
#define VMM_USER     (1ULL << 2)
#define VMM_PWT      (1ULL << 3)   /* page write-through (cache hint)   */
#define VMM_PCD      (1ULL << 4)   /* page cache disable (MMIO regions) */
/* Bits 9-11 are reserved by the architecture for OS use. We use bit 9 to
 * mark PTEs whose phys frame is borrowed from another task via shmem_share:
 * the receiving task must NOT pmm_free_frame these on exit, because the
 * owner (e.g. winman) still has them mapped and on its malloc free list. */
#define VMM_SHARED   (1ULL << 9)
#define VMM_NX       (1ULL << 63)

void     vmm_init(void);
int      vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int      vmm_unmap(uint64_t virt);
uint64_t vmm_translate(uint64_t virt);   // returns 0 if unmapped

/* Per-PML4 variants. Operate on the supplied PML4 (physical addr cast to ptr,
 * works because pmm allocates from identity-mapped low memory). The non-_in
 * versions are wrappers that pass the boot kernel_pml4. */
int      vmm_map_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
int      vmm_unmap_in(uint64_t *pml4, uint64_t virt);
uint64_t vmm_translate_in(uint64_t *pml4, uint64_t virt);

#endif
