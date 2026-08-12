/* Host test for production free_user_pml4 ownership and hierarchy rules. */
#include "memory/vmm.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_FRAMES 64
#define PAGE_SIZE 4096

static uint8_t physical[TEST_FRAMES][PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static uint8_t freed[TEST_FRAMES];
static int next_frame = 1;
static int duplicate_free;
static int failures;

void free_user_pml4(uint64_t *pml4);

void *hhdm_test_phys_to_virt(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame == 0 || frame >= TEST_FRAMES)
        return 0;
    return physical[frame] + (phys & (PAGE_SIZE - 1));
}

uint64_t hhdm_test_virt_to_phys(const void *virt) {
    const uint8_t *ptr = virt;
    for (int i = 1; i < TEST_FRAMES; i++) {
        if (ptr >= physical[i] && ptr < physical[i] + PAGE_SIZE)
            return (uint64_t)i * PAGE_SIZE + (uint64_t)(ptr - physical[i]);
    }
    return 0;
}

void pmm_free_frame(uint64_t phys) {
    int frame = (int)(phys / PAGE_SIZE);
    if (frame <= 0 || frame >= TEST_FRAMES) {
        duplicate_free = 1;
        return;
    }
    if (freed[frame])
        duplicate_free = 1;
    freed[frame] = 1;
}

static uint64_t alloc_frame(void) {
    uint64_t phys = (uint64_t)next_frame++ * PAGE_SIZE;
    memset(hhdm_test_phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

static uint64_t *table(uint64_t phys) {
    return hhdm_test_phys_to_virt(phys);
}

static void expect(int condition, const char *message) {
    if (condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void expect_freed(uint64_t phys, int should_be_freed,
                         const char *message) {
    expect(freed[phys / PAGE_SIZE] == should_be_freed, message);
}

int main(void) {
    uint64_t pml4_phys = alloc_frame();
    uint64_t *pml4 = table(pml4_phys);

    uint64_t pdpt0_phys = alloc_frame();
    uint64_t *pdpt0 = table(pdpt0_phys);
    pml4[0] = pdpt0_phys | VMM_PRESENT;

    uint64_t kernel_low_pd = alloc_frame();
    pdpt0[0] = kernel_low_pd | VMM_PRESENT;

    uint64_t pd0_phys = alloc_frame();
    uint64_t pt0_phys = alloc_frame();
    uint64_t owned0 = alloc_frame();
    uint64_t borrowed0 = alloc_frame();
    pdpt0[1] = pd0_phys | VMM_PRESENT;
    table(pd0_phys)[3] = pt0_phys | VMM_PRESENT;
    table(pt0_phys)[4] = owned0 | VMM_PRESENT | VMM_USER;
    table(pt0_phys)[5] = borrowed0 | VMM_PRESENT | VMM_USER | VMM_SHARED;

    uint64_t pml4_1_pdpt = alloc_frame();
    uint64_t pml4_1_pd = alloc_frame();
    uint64_t pml4_1_pt = alloc_frame();
    uint64_t owned1 = alloc_frame();
    pml4[1] = pml4_1_pdpt | VMM_PRESENT;
    table(pml4_1_pdpt)[2] = pml4_1_pd | VMM_PRESENT;
    table(pml4_1_pd)[7] = pml4_1_pt | VMM_PRESENT;
    table(pml4_1_pt)[9] = owned1 | VMM_PRESENT | VMM_USER;

    uint64_t huge_user_frame = alloc_frame();
    table(pml4_1_pdpt)[8] = huge_user_frame | VMM_PRESENT | VMM_PS;

    uint64_t high_pdpt = alloc_frame();
    uint64_t high_leaf = alloc_frame();
    pml4[256] = high_pdpt | VMM_PRESENT;
    table(high_pdpt)[0] = high_leaf | VMM_PRESENT | VMM_PS;

    free_user_pml4(pml4);

    expect_freed(pml4_phys, 1, "process PML4 frame is freed");
    expect_freed(pdpt0_phys, 1, "private low-half PDPT is freed");
    expect_freed(pd0_phys, 1, "private PD is freed");
    expect_freed(pt0_phys, 1, "private PT is freed");
    expect_freed(owned0, 1, "owned low-half leaf is freed");
    expect_freed(borrowed0, 0, "borrowed shared leaf is retained");
    expect_freed(kernel_low_pd, 0, "shared low identity subtree is retained");

    expect_freed(pml4_1_pdpt, 1, "secondary user PDPT is freed");
    expect_freed(pml4_1_pd, 1, "secondary user PD is freed");
    expect_freed(pml4_1_pt, 1, "secondary user PT is freed");
    expect_freed(owned1, 1, "secondary owned leaf is freed");
    expect_freed(huge_user_frame, 0, "huge mapping is not treated as a PT");
    expect_freed(high_pdpt, 0, "kernel high-half tables are retained");
    expect_freed(high_leaf, 0, "kernel high-half mappings are retained");
    expect(!duplicate_free, "no frame is freed twice");

    int before = duplicate_free;
    free_user_pml4(0);
    expect(duplicate_free == before, "null teardown is a no-op");

    if (failures)
        return 1;
    puts("process_pml4_test: all checks passed");
    return 0;
}
