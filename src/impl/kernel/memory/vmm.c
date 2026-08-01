/* src/impl/kernel/memory/vmm.c — long-mode 4-level paging.
 *
 * walk_or_create grows the tree as needed during map; walk_only never
 * allocates (used by unmap so we don't leak intermediate tables for an
 * address that wasn't actually mapped — the previous version did, which
 * was a quiet leak waiting for a fuzz test to find it).
 *
 * The non-_in wrappers operate on whichever PML4 is currently in CR3 —
 * the natural thing for callers that want their effect visible to the
 * running context. Kernel-shared subtrees are linked by physical address
 * into every process PML4, so writes there propagate to kernel_pml4 too.
 */
#include "memory/vmm.h"
#include "memory/hhdm.h"
#include "memory/pmm.h"
#include "arch/cpu.h"
#include "utilities/string.h"
#include "utilities/log.h"
#include <stdint.h>

#define PAGE_SIZE 4096
#define ENTRIES_PER_TABLE 512
#define ADDR_MASK 0x000FFFFFFFFFF000ULL    /* bits 12-51 = frame addr */
#define PAGE_PS   (1ULL << 7)              /* huge-page bit (PDPT=1GiB, PD=2MiB) */

uint64_t *kernel_pml4 = 0;

/* Crack a virtual address into its four index slices. Inlined because
 * doing this by hand four times across three functions was where the
 * bugs lived. */
static inline void va_split(uint64_t virt, uint64_t idx[4]) {
    idx[0] = (virt >> 39) & 0x1FF;   /* PML4 */
    idx[1] = (virt >> 30) & 0x1FF;   /* PDPT */
    idx[2] = (virt >> 21) & 0x1FF;   /* PD   */
    idx[3] = (virt >> 12) & 0x1FF;   /* PT   */
}

/* Walk down to the leaf PTE, allocating + zeroing intermediate tables on
 * demand. Propagates the VMM_USER bit upward so user mappings don't get
 * blocked by a kernel-only upper level. Returns 0 on success and writes
 * the PTE address to *pte_out, -1 on OOM or on hitting a huge-page
 * short-circuit. */
static int walk_or_create(uint64_t *pml4, uint64_t virt, uint64_t flags,
                          uint64_t **pte_out) {
    uint64_t i[4]; va_split(virt, i);

    uint64_t *pdpt;
    if (pml4[i[0]] & VMM_PRESENT) {
        if ((flags & VMM_USER) && !(pml4[i[0]] & VMM_USER)) {
            pml4[i[0]] |= VMM_USER;
        }
        pdpt = phys_to_virt(pml4[i[0]] & ADDR_MASK);
    } else {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) return -1;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        pml4[i[0]] = phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
        pdpt = phys_to_virt(phys);
    }

    uint64_t *pd;
    if (pdpt[i[1]] & VMM_PRESENT) {
        if (pdpt[i[1]] & PAGE_PS) return -1;  /* 1 GiB huge — can't sub-divide */
        if ((flags & VMM_USER) && !(pdpt[i[1]] & VMM_USER)) {
            pdpt[i[1]] |= VMM_USER;
        }
        pd = phys_to_virt(pdpt[i[1]] & ADDR_MASK);
    } else {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) return -1;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        pdpt[i[1]] = phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
        pd = phys_to_virt(phys);
    }

    uint64_t *pt;
    if (pd[i[2]] & VMM_PRESENT) {
        if (pd[i[2]] & PAGE_PS) return -1;     /* 2 MiB huge — boot uses these */
        if ((flags & VMM_USER) && !(pd[i[2]] & VMM_USER)) {
            pd[i[2]] |= VMM_USER;
        }
        pt = phys_to_virt(pd[i[2]] & ADDR_MASK);
    } else {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) return -1;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        pd[i[2]] = phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
        pt = phys_to_virt(phys);
    }

    *pte_out = &pt[i[3]];
    return 0;
}

/* Walk-only: never allocates intermediate tables. Returns the PTE
 * address if the full path exists (and no huge-page short-circuits along
 * the way), or 0 if any level is missing. */
static uint64_t *walk_only(uint64_t *pml4, uint64_t virt) {
    uint64_t i[4]; va_split(virt, i);

    uint64_t e = pml4[i[0]];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(e & ADDR_MASK);
    e = pdpt[i[1]];
    if (!(e & VMM_PRESENT) || (e & PAGE_PS)) return 0;
    uint64_t *pd = phys_to_virt(e & ADDR_MASK);
    e = pd[i[2]];
    if (!(e & VMM_PRESENT) || (e & PAGE_PS)) return 0;
    uint64_t *pt = phys_to_virt(e & ADDR_MASK);
    return &pt[i[3]];
}

int vmm_map_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte;
    if (walk_or_create(pml4, virt, flags, &pte) != 0) return -1;
    *pte = (phys & ADDR_MASK) | (flags | VMM_PRESENT);
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

int vmm_unmap_in(uint64_t *pml4, uint64_t virt) {
    uint64_t *pte = walk_only(pml4, virt);
    if (!pte) return -1;
    *pte = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

/* Walk to the leaf and return the physical address, honouring 1 GiB
 * and 2 MiB huge-page short-circuits along the way. Returns 0 if any
 * level is unmapped. */
uint64_t vmm_translate_in(uint64_t *pml4, uint64_t virt) {
    uint64_t i[4]; va_split(virt, i);

    uint64_t e = pml4[i[0]];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(e & ADDR_MASK);
    e = pdpt[i[1]];
    if (!(e & VMM_PRESENT)) return 0;
    if (e & PAGE_PS) return (e & ADDR_MASK) | (virt & 0x3FFFFFFF);   /* 1 GiB */
    uint64_t *pd = phys_to_virt(e & ADDR_MASK);
    e = pd[i[2]];
    if (!(e & VMM_PRESENT)) return 0;
    if (e & PAGE_PS) return (e & ADDR_MASK) | (virt & 0x1FFFFF);     /* 2 MiB */
    uint64_t *pt = phys_to_virt(e & ADDR_MASK);
    e = pt[i[3]];
    if (!(e & VMM_PRESENT)) return 0;
    return (e & ADDR_MASK) | (virt & 0xFFF);
}

/* Read CR3 and strip the flags to get the active PML4 base. */
static inline uint64_t *current_pml4(void) {
    return phys_to_virt(read_cr3() & ADDR_MASK);
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return vmm_map_in(current_pml4(), virt, phys, flags);
}

int vmm_unmap(uint64_t virt) {
    return vmm_unmap_in(current_pml4(), virt);
}

uint64_t vmm_translate(uint64_t virt) {
    return vmm_translate_in(current_pml4(), virt);
}

void vmm_init(void) {
    /* Grab the boot PML4 from CR3 — that's the table set up by main.asm. */
    kernel_pml4 = phys_to_virt(read_cr3() & ADDR_MASK);
    log_write("VMM: using boot PML4", KERNEL, LOG_INFO);
}
