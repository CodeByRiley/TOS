/* kernel/memory/vmm.h — virtual-memory mapper.
 *
 * Long-mode 4-level paging. Standard PTE bits exposed as VMM_*, plus a
 * project-specific VMM_SHARED bit (PTE bit 9) marking pages that were
 * shared in via shmem_share — those must NOT be freed back to the PMM
 * on task exit because the owner still has them mapped.
 *
 * Implementation: kernel/memory/vmm.c.
 */
#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define VMM_PRESENT  (1ULL << 0)
#define VMM_WRITE    (1ULL << 1)
#define VMM_USER     (1ULL << 2)
#define VMM_PWT      (1ULL << 3)   /* page write-through (cache hint)   */
#define VMM_PCD      (1ULL << 4)   /* page cache disable (MMIO regions) */
/* Bit 9 is reserved by the architecture for OS use. We use it to mark
 * PTEs whose phys frame is borrowed via shmem_share: the receiving task
 * must NOT pmm_free_frame these on exit, because the owner (e.g. winman)
 * still has them mapped and on its malloc free list. */
#define VMM_SHARED   (1ULL << 9)
#define VMM_NX       (1ULL << 63)

void     vmm_init(void);

/* Operate on the boot kernel_pml4. */
int      vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int      vmm_unmap(uint64_t virt);
uint64_t vmm_translate(uint64_t virt);   /* returns 0 if unmapped */

/* Per-PML4 variants. Operate on the supplied PML4 as an HHDM pointer;
 * physical addresses remain physical in the entries themselves.
 * The non-_in versions are wrappers that pass kernel_pml4. */
int      vmm_map_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
int      vmm_unmap_in(uint64_t *pml4, uint64_t virt);
uint64_t vmm_translate_in(uint64_t *pml4, uint64_t virt);

/* Change permissions on an already-mapped page, keeping its frame and its
 * VMM_SHARED ownership bit. Returns -1 if the page is not mapped. */
int      vmm_protect_in(uint64_t *pml4, uint64_t virt, uint64_t flags);

/* Raw leaf PTE (frame + flags), or 0 if unmapped. Callers mask with
 * ~0xFFF for the frame and test VMM_* for the flags. */
uint64_t vmm_entry_in(uint64_t *pml4, uint64_t virt);

#endif
