/* kernel/memory/vmm.h — virtual-memory mapper.
 *
 * Long-mode 4-level paging. Standard PTE bits exposed as VMM_*, plus a
 * project-specific VMM_SHARED bit (PTE bit 9) marking borrowed pages —
 * those must NOT be freed back to the PMM on task exit because another
 * subsystem or process owns them.
 *
 * Implementation: kernel/memory/vmm.c.
 */
#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Page table entry (all four levels share this layout) */
//
// Bits  | Name                | Description
// 63    | NX                  | 1 = No execute (needs EFER.NXE)
// 62-52 | Available           | Ignored by the CPU, free for software use
// 51-12 | Physical Address    | Frame address; must be 4KB aligned
// 11-9  | Available           | Ignored by the CPU — VMM_SHARED lives at bit 9
// 8     | Global              | 1 = Entry survives a CR3 reload (needs CR4.PGE)
// 7     | PS / PAT            | Page Size at PD/PDPT level; PAT at PT level
// 6     | Dirty               | Set by the CPU on write; never cleared by it
// 5     | Accessed            | Set by the CPU on any access
// 4     | PCD                 | 1 = Cache disabled — required for MMIO
// 3     | PWT                 | 1 = Write-through instead of write-back
// 2     | User                | 1 = Ring 3 may access; 0 = supervisor only
// 1     | Write               | 1 = Writable (read-only if clear)
// 0     | Present             | 0 = Entry unused; every other bit is then free
//
// Access and Dirty are CPU-written, so a PTE read back may differ from what
// was stored. Preserve them on read-modify-write rather than rebuilding an
// entry from flags alone.
#define VMM_PRESENT  (1ULL << 0)
#define VMM_WRITE    (1ULL << 1)
#define VMM_USER     (1ULL << 2)
#define VMM_PWT      (1ULL << 3)
#define VMM_PCD      (1ULL << 4)
#define VMM_ACCESSED (1ULL << 5)
#define VMM_DIRTY    (1ULL << 6)
#define VMM_PS       (1ULL << 7)
#define VMM_GLOBAL   (1ULL << 8)
/* Bit 9 is one of the three software-available bits. It marks a non-owning
 * mapping, including shmem receivers and userspace framebuffer mappings.
 * Teardown removes the PTE but must not return the frame to the PMM. */
#define VMM_SHARED   (1ULL << 9)
#define VMM_NX       (1ULL << 63)

/* Frame address occupies bits 51-12; everything else is flags. */
#define VMM_ADDR_MASK 0x000FFFFFFFFFF000ULL

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
