#include "memory/vmm.h"
#include "memory/pmm.h"
#include "utilities/string.h"
#include "utilities/log.h"
#include <stdint.h>

#define PAGE_SIZE 4096
#define ENTRIES_PER_TABLE 512
#define ADDR_MASK 0x000FFFFFFFFFF000ULL    // bits 12-51 = frame addr

static uint64_t *kernel_pml4 = 0;

static uint64_t *table_at_entry(uint64_t entry, uint64_t flags) {
    if (entry & VMM_PRESENT) {
        return (uint64_t*)(entry & ADDR_MASK);
    }
    // alloc new table
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    uint64_t *tbl = (uint64_t*)phys;
    memset(tbl, 0, PAGE_SIZE);
    return tbl;
}

static int walk_or_create(uint64_t virt, uint64_t flags, uint64_t **pte_out) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = kernel_pml4;
    uint64_t *pdpt;
    if (pml4[pml4_idx] & VMM_PRESENT) {
        pdpt = (uint64_t*)(pml4[pml4_idx] & ADDR_MASK);
    } else {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) return -1;
        memset((void*)phys, 0, PAGE_SIZE);
        pml4[pml4_idx] = phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
        pdpt = (uint64_t*)phys;
    }

    uint64_t *pd;
    if (pdpt[pdpt_idx] & VMM_PRESENT) {
        if (pdpt[pdpt_idx] & (1ULL << 7)) return -1;  // 1GiB huge, can't sub-divide
        pd = (uint64_t*)(pdpt[pdpt_idx] & ADDR_MASK);
    } else {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) return -1;
        memset((void*)phys, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
        pd = (uint64_t*)phys;
    }

    uint64_t *pt;
    if (pd[pd_idx] & VMM_PRESENT) {
        if (pd[pd_idx] & (1ULL << 7)) return -1;      // 2MiB huge — boot uses these
        pt = (uint64_t*)(pd[pd_idx] & ADDR_MASK);
    } else {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) return -1;
        memset((void*)phys, 0, PAGE_SIZE);
        pd[pd_idx] = phys | VMM_PRESENT | VMM_WRITE | (flags & VMM_USER);
        pt = (uint64_t*)phys;
    }

    *pte_out = &pt[pt_idx];
    return 0;
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte;
    if (walk_or_create(virt, flags, &pte) != 0) return -1;
    *pte = (phys & ADDR_MASK) | (flags | VMM_PRESENT);
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

int vmm_unmap(uint64_t virt) {
    uint64_t *pte;
    if (walk_or_create(virt, 0, &pte) != 0) return -1;
    *pte = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
    return 0;
}

uint64_t vmm_translate(uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t e = kernel_pml4[pml4_idx];
    if (!(e & VMM_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t*)(e & ADDR_MASK);
    e = pdpt[pdpt_idx];
    if (!(e & VMM_PRESENT)) return 0;
    if (e & (1ULL << 7)) return (e & ADDR_MASK) | (virt & 0x3FFFFFFF);   // 1GiB
    uint64_t *pd = (uint64_t*)(e & ADDR_MASK);
    e = pd[pd_idx];
    if (!(e & VMM_PRESENT)) return 0;
    if (e & (1ULL << 7)) return (e & ADDR_MASK) | (virt & 0x1FFFFF);     // 2MiB
    uint64_t *pt = (uint64_t*)(e & ADDR_MASK);
    e = pt[pt_idx];
    if (!(e & VMM_PRESENT)) return 0;
    return (e & ADDR_MASK) | (virt & 0xFFF);
}

void vmm_init(void) {
    // grab boot PML4 from CR3
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = (uint64_t*)(cr3 & ADDR_MASK);
    log_write("VMM: using boot PML4", KERNEL, LOG_INFO);
}
