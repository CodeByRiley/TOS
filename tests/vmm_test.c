/* Host stress test for the production four-level VMM page-table walker. */
#include "memory/vmm.h"
#include "memory/pmm.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_FRAMES 512
#define PAGE_SIZE 4096

static uint8_t physical[TEST_FRAMES][PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static uint8_t allocated[TEST_FRAMES];
static int allocations;
static int failures;

void *hhdm_test_phys_to_virt(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame == 0 || frame >= TEST_FRAMES)
        return 0;
    return physical[frame] + (phys & (PAGE_SIZE - 1));
}

uint64_t hhdm_test_virt_to_phys(const void *virt) {
    const uint8_t *ptr = virt;
    for (uint64_t i = 1; i < TEST_FRAMES; i++) {
        if (ptr >= physical[i] && ptr < physical[i] + PAGE_SIZE)
            return i * PAGE_SIZE + (uint64_t)(ptr - physical[i]);
    }
    return 0;
}

uint64_t pmm_alloc_frame(void) {
    for (int i = 1; i < TEST_FRAMES; i++) {
        if (allocated[i])
            continue;
        allocated[i] = 1;
        memset(physical[i], 0, PAGE_SIZE);
        allocations++;
        return (uint64_t)i * PAGE_SIZE;
    }
    return 0;
}

void log_write(const char *message, uint8_t type, uint8_t level) {
    (void)message;
    (void)type;
    (void)level;
}

static void expect(int condition, const char *message) {
    if (condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static uint64_t *new_pml4(void) {
    memset(physical, 0, sizeof(physical));
    memset(allocated, 0, sizeof(allocated));
    allocations = 0;
    return hhdm_test_phys_to_virt(pmm_alloc_frame());
}

static uint64_t *next_table(uint64_t entry) {
    return hhdm_test_phys_to_virt(entry & VMM_ADDR_MASK);
}

static void test_map_translate_protect(void) {
    uint64_t *pml4 = new_pml4();
    uint64_t va = 0x0000000123456000ULL;
    uint64_t phys = 0x0000000000ABC000ULL;

    expect(vmm_map_in(pml4, va, phys, VMM_WRITE) == 0, "map a supervisor page");
    expect(allocations == 4, "first mapping allocates three table levels");
    expect(vmm_translate_in(pml4, va + 0x321) == phys + 0x321,
           "translation preserves the page offset");

    uint64_t i0 = (va >> 39) & 0x1FF;
    uint64_t i1 = (va >> 30) & 0x1FF;
    uint64_t i2 = (va >> 21) & 0x1FF;
    uint64_t *pdpt = next_table(pml4[i0]);
    uint64_t *pd = next_table(pdpt[i1]);
    expect(!(pml4[i0] & VMM_USER) && !(pdpt[i1] & VMM_USER) &&
           !(pd[i2] & VMM_USER), "supervisor mapping keeps upper levels private");

    expect(vmm_map_in(pml4, va + PAGE_SIZE, phys + PAGE_SIZE,
                      VMM_WRITE | VMM_USER | VMM_SHARED) == 0,
           "map a shared user page in the existing branch");
    expect(allocations == 4, "sibling mapping reuses page tables");
    expect((pml4[i0] & VMM_USER) && (pdpt[i1] & VMM_USER) &&
           (pd[i2] & VMM_USER), "user permission propagates through every level");

    uint64_t shared_va = va + PAGE_SIZE;
    expect(vmm_protect_in(pml4, shared_va, VMM_USER | VMM_NX) == 0,
           "protect an existing mapping");
    uint64_t entry = vmm_entry_in(pml4, shared_va);
    expect((entry & VMM_ADDR_MASK) == phys + PAGE_SIZE,
           "protect preserves the physical frame");
    expect((entry & VMM_SHARED) && (entry & VMM_NX) && !(entry & VMM_WRITE),
           "protect preserves ownership while replacing permissions");

    expect(vmm_unmap_in(pml4, shared_va) == 0, "unmap an existing branch");
    expect(vmm_translate_in(pml4, shared_va) == 0, "unmapped page no longer translates");
    int before = allocations;
    expect(vmm_unmap_in(pml4, 0x0000600000000000ULL) != 0,
           "unmap rejects an absent branch");
    expect(allocations == before, "failed unmap never allocates page tables");
}

static void test_huge_pages_and_rejections(void) {
    uint64_t *pml4 = new_pml4();
    uint64_t va_1g = 0x0000008040123456ULL;
    uint64_t pml4_i = (va_1g >> 39) & 0x1FF;
    uint64_t pdpt_i = (va_1g >> 30) & 0x1FF;
    uint64_t pdpt_phys = pmm_alloc_frame();
    uint64_t *pdpt = hhdm_test_phys_to_virt(pdpt_phys);
    pml4[pml4_i] = pdpt_phys | VMM_PRESENT | VMM_WRITE;
    pdpt[pdpt_i] = 0x0000000100000000ULL | VMM_PRESENT | VMM_PS;
    expect(vmm_translate_in(pml4, va_1g) ==
           0x0000000100000000ULL + (va_1g & 0x3FFFFFFFULL),
           "translate through a 1 GiB huge page");
    expect(vmm_map_in(pml4, va_1g, 0x200000, VMM_WRITE) != 0,
           "mapping cannot subdivide a 1 GiB huge page");

    uint64_t va_2m = 0x0000010000612345ULL;
    pml4_i = (va_2m >> 39) & 0x1FF;
    pdpt_i = (va_2m >> 30) & 0x1FF;
    uint64_t pd_i = (va_2m >> 21) & 0x1FF;
    pdpt_phys = pmm_alloc_frame();
    pdpt = hhdm_test_phys_to_virt(pdpt_phys);
    uint64_t pd_phys = pmm_alloc_frame();
    uint64_t *pd = hhdm_test_phys_to_virt(pd_phys);
    pml4[pml4_i] = pdpt_phys | VMM_PRESENT;
    pdpt[pdpt_i] = pd_phys | VMM_PRESENT;
    pd[pd_i] = 0x0000000200400000ULL | VMM_PRESENT | VMM_PS;
    expect(vmm_translate_in(pml4, va_2m) ==
           0x0000000200400000ULL + (va_2m & 0x1FFFFFULL),
           "translate through a 2 MiB huge page");

    int before = allocations;
    expect(vmm_map_in(pml4, 0x4000, 0xFFFF800000001000ULL,
                      VMM_USER) != 0,
           "canonical kernel virtual address is rejected as physical input");
    expect(allocations == before, "invalid physical input allocates nothing");
}

static void test_mapping_churn(void) {
    uint64_t *pml4 = new_pml4();
    for (int i = 0; i < 2048; i++) {
        uint64_t va = 0x0000000200000000ULL + (uint64_t)i * PAGE_SIZE;
        uint64_t phys = 0x0000000040000000ULL + (uint64_t)i * PAGE_SIZE;
        expect(vmm_map_in(pml4, va, phys, VMM_USER | VMM_WRITE) == 0,
               "bulk mapping succeeds");
    }
    for (int i = 0; i < 2048; i++) {
        uint64_t va = 0x0000000200000000ULL + (uint64_t)i * PAGE_SIZE;
        uint64_t phys = 0x0000000040000000ULL + (uint64_t)i * PAGE_SIZE;
        expect(vmm_translate_in(pml4, va + (i & 0xFFF)) == phys + (i & 0xFFF),
               "bulk mapping translates correctly");
    }
    for (int i = 1; i < 2048; i += 2)
        expect(vmm_unmap_in(pml4, 0x0000000200000000ULL +
                            (uint64_t)i * PAGE_SIZE) == 0,
               "bulk odd-page unmap succeeds");
    for (int i = 0; i < 2048; i++) {
        uint64_t va = 0x0000000200000000ULL + (uint64_t)i * PAGE_SIZE;
        expect((vmm_translate_in(pml4, va) != 0) == ((i & 1) == 0),
               "bulk sparse translation state is correct");
    }
}

int main(void) {
    test_map_translate_protect();
    test_huge_pages_and_rejections();
    test_mapping_churn();
    if (failures)
        return 1;
    puts("vmm_test: all checks passed");
    return 0;
}
