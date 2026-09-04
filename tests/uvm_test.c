/* Host test for the user address space , kernel/memory/uvm.c.
 *
 * uvm takes a struct task_vm and nothing else, so the whole module runs here
 * against a fake PMM and a fake page table. That is the point of the seam:
 * the reservation table, the arena and the demand-paging rules are exercised
 * directly instead of through a booted kernel and a syscall.
 *
 * The fakes are deliberately assertable. Frame accounting is checked, not
 * just tolerated, because several of these behaviours are about *not*
 * allocating.
 */
#include "memory/uvm.h"

#include "memory/vmm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

/* --- fake PMM ---------------------------------------------------------- */

#define POOL_BASE 0x0000000000100000ULL
#define POOL_FRAMES 64

static uint8_t frame_store[POOL_FRAMES][4096];
static int frame_taken[POOL_FRAMES];
static int frames_in_use;
static int alloc_calls;
static int alloc_fail_after = -1; /* -1 disables the injected failure */

u64 pmm_alloc_frame(void) {
    if (alloc_fail_after >= 0 && alloc_calls >= alloc_fail_after) {
        alloc_calls++;
        return 0;
    }
    for (int i = 0; i < POOL_FRAMES; i++) {
        if (frame_taken[i])
            continue;
        frame_taken[i] = 1;
        frames_in_use++;
        alloc_calls++;
        return POOL_BASE + (u64)i * 4096;
    }
    alloc_calls++;
    return 0;
}

void pmm_free_frame(u64 phys) {
    u64 index = (phys - POOL_BASE) / 4096;
    if (phys < POOL_BASE || index >= POOL_FRAMES) {
        fprintf(stderr, "FAIL: pmm_free_frame given %llx, outside the pool\n",
                (unsigned long long)phys);
        failures++;
        return;
    }
    if (!frame_taken[index]) {
        fprintf(stderr, "FAIL: double free of frame %llu\n",
                (unsigned long long)index);
        failures++;
        return;
    }
    frame_taken[index] = 0;
    frames_in_use--;
}

void *hhdm_test_phys_to_virt(u64 phys) {
    return &frame_store[(phys - POOL_BASE) / 4096][0];
}

u64 hhdm_test_virt_to_phys(const void *virt) {
    return POOL_BASE + (u64)((const uint8_t *)virt - &frame_store[0][0]);
}

/* --- fake page table --------------------------------------------------- */

#define PT_SLOTS 512

static struct {
    u64 va;
    u64 entry;
} page_table[PT_SLOTS];

static int pt_find(u64 va) {
    for (int i = 0; i < PT_SLOTS; i++) {
        if (page_table[i].entry && page_table[i].va == va)
            return i;
    }
    return -1;
}

int vmm_map_in(u64 *pml4, u64 virt, u64 phys, u64 flags) {
    (void)pml4;
    if (pt_find(virt) >= 0)
        return -1;
    for (int i = 0; i < PT_SLOTS; i++) {
        if (page_table[i].entry)
            continue;
        page_table[i].va = virt;
        page_table[i].entry = phys | flags;
        return 0;
    }
    return -1;
}

int vmm_unmap_in(u64 *pml4, u64 virt) {
    (void)pml4;
    int i = pt_find(virt);
    if (i < 0)
        return -1;
    page_table[i].entry = 0;
    return 0;
}

u64 vmm_entry_in(u64 *pml4, u64 virt) {
    (void)pml4;
    int i = pt_find(virt);
    return i < 0 ? 0 : page_table[i].entry;
}

u64 vmm_translate_in(u64 *pml4, u64 virt) {
    (void)pml4;
    int i = pt_find(virt);
    return i < 0 ? 0 : (page_table[i].entry & VMM_ADDR_MASK);
}

int vmm_protect_in(u64 *pml4, u64 virt, u64 flags) {
    (void)pml4;
    int i = pt_find(virt);
    if (i < 0)
        return -1;
    page_table[i].entry = (page_table[i].entry & VMM_ADDR_MASK) | flags;
    return 0;
}

void log_write(const char *message, uint8_t type, uint8_t level) {
    (void)message;
    (void)type;
    (void)level;
}

/* --- fixtures ---------------------------------------------------------- */

#define RW (VMM_PRESENT | VMM_USER | VMM_WRITE | VMM_NX)
#define RO (VMM_PRESENT | VMM_USER | VMM_NX)

static u64 fake_pml4_storage;
static struct task_vm vm;

static void reset(void) {
    memset(frame_taken, 0, sizeof(frame_taken));
    memset(frame_store, 0xAB, sizeof(frame_store));
    memset(page_table, 0, sizeof(page_table));
    frames_in_use = 0;
    alloc_calls = 0;
    alloc_fail_after = -1;

    memset(&vm, 0, sizeof(vm));
    vm.user_pml4 = &fake_pml4_storage;
    uvm_init(&vm);
}

/* --- reservations ------------------------------------------------------ */

static void test_init_places_both_arenas(void) {
    reset();
    expect(vm.mmap_next_va == USER_MMAP_BASE, "init puts the mmap arena at its base");
    expect(vm.shmem_next_va == USER_SHMEM_BASE, "init puts the shmem arena at its base");
    expect(uvm_reserve(&vm, 0, 4096, RW, 0) == USER_MMAP_BASE,
           "the first auto-placed reservation is the arena base");
}

static void test_auto_placement_bumps(void) {
    reset();
    u64 a = uvm_reserve(&vm, 0, 4096, RW, 0);
    u64 b = uvm_reserve(&vm, 0, 8192, RW, 0);
    expect(b == a + 4096, "auto placement bumps past the previous reservation");
    expect(uvm_reserve(&vm, 0, 4096, RW, 0) == b + 8192,
           "the bump accounts for the whole previous length");
    expect(frames_in_use == 0, "reserving allocates no frames");
}

static void test_reserve_rejects_bad_ranges(void) {
    reset();
    expect(uvm_reserve(&vm, 0, 0, RW, 0) == 0, "a zero-length reservation fails");
    expect(uvm_reserve(&vm, 0, 4097, RW, 0) == 0,
           "an unrounded length fails rather than being rounded here");
    expect(uvm_reserve(&vm, USER_MMAP_BASE + 1, 4096, RW, 1) == 0,
           "a fixed reservation at an unaligned address fails");
    expect(uvm_reserve(&vm, 0x800, 4096, RW, 1) == 0,
           "a fixed reservation below USER_VA_MIN fails");
    expect(uvm_reserve(&vm, USER_STACK_LOW, 4096, RW, 1) == 0,
           "a fixed reservation cannot land on the stack");
    expect(uvm_reserve(&vm, USER_MMAP_BASE, 4096, 0, 1) == 0,
           "a reservation with no page flags fails");
}

static void test_fixed_rejects_occupied_range(void) {
    reset();
    u64 base = 0x0000000100000000ULL;
    expect(uvm_reserve(&vm, base, 8192, RW, 1) == base, "a legal fixed reservation succeeds");
    expect(uvm_reserve(&vm, base + 4096, 4096, RW, 1) == 0,
           "a fixed reservation overlapping an existing one fails");
    expect(uvm_reserve(&vm, base + 8192, 4096, RW, 1) == base + 8192,
           "a fixed reservation abutting an existing one succeeds");
}

static void test_reservation_table_is_bounded(void) {
    reset();
    for (int i = 0; i < MAX_USER_VMAS; i++) {
        expect(uvm_reserve(&vm, 0, 4096, RW, 0) != 0, "reservations up to the table size succeed");
    }
    expect(uvm_reserve(&vm, 0, 4096, RW, 0) == 0,
           "the reservation past the table size fails instead of overrunning it");
}

/* --- demand paging ----------------------------------------------------- */

static void test_fault_in_materialises_zeroed_page(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 4096, RW, 0);
    expect(uvm_fault_in(&vm, base + 17, 1) == 1,
           "a write inside a writable reservation is served");
    expect(frames_in_use == 1, "serving it took exactly one frame");

    u64 entry = vmm_entry_in(vm.user_pml4, base);
    expect((entry & VMM_WRITE) != 0, "the page carries the reservation's flags");
    const uint8_t *page = hhdm_test_phys_to_virt(entry & VMM_ADDR_MASK);
    int zeroed = 1;
    for (int i = 0; i < 4096; i++)
        if (page[i])
            zeroed = 0;
    expect(zeroed, "the materialised page is zeroed, not left as prior contents");
}

/* The behaviour the fault handler and the syscall path used to disagree on.
 * uvm refuses, so the task reaches its segfault one fault earlier and no
 * frame is spent on a page it is not allowed to write. */
static void test_write_to_readonly_reservation_is_refused(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 4096, RO, 0);

    expect(uvm_fault_in(&vm, base, 1) == 0,
           "a write into a read-only reservation is refused");
    expect(frames_in_use == 0, "the refused write allocated no frame");
    expect(vmm_entry_in(vm.user_pml4, base) == 0, "and mapped nothing");

    expect(uvm_fault_in(&vm, base, 0) == 1, "a read of the same page is served");
    expect(frames_in_use == 1, "the read took one frame");
    expect(uvm_fault_in(&vm, base, 1) == 0,
           "a write to the now-present read-only page is still refused");
    expect(frames_in_use == 1, "and did not allocate a second frame");
}

static void test_fault_outside_any_reservation(void) {
    reset();
    uvm_reserve(&vm, 0, 4096, RW, 0);
    expect(uvm_fault_in(&vm, USER_MMAP_BASE + 4096, 0) == 0,
           "a fault just past a reservation is not served");
    expect(uvm_fault_in(0, USER_MMAP_BASE, 0) == 0,
           "a fault against a null address space is not served");
    expect(frames_in_use == 0, "neither allocated a frame");
}

static void test_fault_in_releases_frame_when_mapping_fails(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 4096, RW, 0);
    for (int i = 0; i < PT_SLOTS; i++) {
        page_table[i].va = 0xDEAD0000ULL + (u64)i * 4096;
        page_table[i].entry = 1;
    }
    expect(uvm_fault_in(&vm, base, 1) == 0, "a failed mapping reports failure");
    expect(frames_in_use == 0, "and returns the frame it had already taken");
}

/* --- buffers ----------------------------------------------------------- */

static void test_buffer_ok_spans_pages(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 3 * 4096, RW, 0);
    expect(uvm_buffer_ok(&vm, (void *)(uintptr_t)(base + 4000), 5000, 1) == 1,
           "a buffer straddling three pages is accepted");
    expect(frames_in_use == 3, "and every page it covers was materialised");

    expect(uvm_buffer_ok(&vm, (void *)(uintptr_t)base, 0, 1) == 1,
           "a zero-length buffer is accepted without touching anything");
    expect(uvm_buffer_ok(&vm, (void *)0x400, 16, 0) == 0,
           "a buffer below USER_VA_MIN is rejected");
    expect(uvm_buffer_ok(&vm, (void *)(uintptr_t)(base + 3 * 4096), 16, 0) == 0,
           "a buffer past the reservation is rejected");
}

static void test_buffer_ok_admits_the_stack(void) {
    reset();
    /* uvm_range_ok refuses the stack because a reservation must never
     * allocate over it, but an ordinary syscall buffer lives there. */
    u64 sp = USER_STACK_TOP - 4096;
    expect(uvm_range_ok(sp, 64) == 0, "the stack is not reservable");
    vmm_map_in(vm.user_pml4, sp, POOL_BASE, RW);
    expect(uvm_buffer_ok(&vm, (void *)(uintptr_t)sp, 64, 1) == 1,
           "a buffer on an already-mapped stack page is accepted");
}

/* --- release and reuse ------------------------------------------------- */

static void test_release_frees_frames_and_reuses_va(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 2 * 4096, RW, 0);
    uvm_fault_in(&vm, base, 1);
    uvm_fault_in(&vm, base + 4096, 1);
    expect(frames_in_use == 2, "both pages were materialised");

    expect(uvm_release(&vm, base, 2 * 4096) == 0, "releasing the range succeeds");
    expect(frames_in_use == 0, "release returned both frames");
    expect(vmm_entry_in(vm.user_pml4, base) == 0, "and unmapped them");
    expect(uvm_reserve(&vm, 0, 2 * 4096, RW, 0) == base,
           "the freed arena range is handed out again");
}

static void test_release_leaves_borrowed_frames_alone(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 4096, RW, 0);
    /* A page shared in from another address space: mapped here, owned there. */
    vmm_map_in(vm.user_pml4, base, POOL_BASE, RW | VMM_SHARED);
    frame_taken[0] = 1;
    frames_in_use = 1;

    expect(uvm_release(&vm, base, 4096) == 0, "releasing a borrowed page succeeds");
    expect(frames_in_use == 1, "but does not hand the lender's frame to the PMM");
}

static void test_holes_merge(void) {
    reset();
    u64 a = uvm_reserve(&vm, 0, 4096, RW, 0);
    u64 b = uvm_reserve(&vm, 0, 4096, RW, 0);
    u64 c = uvm_reserve(&vm, 0, 4096, RW, 0);
    expect(c == a + 8192, "three consecutive reservations are contiguous");

    expect(uvm_release(&vm, a, 4096) == 0, "the first release succeeds");
    expect(uvm_release(&vm, b, 4096) == 0, "the second release succeeds");
    expect(uvm_reserve(&vm, 0, 8192, RW, 0) == a,
           "two adjacent holes merge into one range large enough to reuse");
}

static void test_release_splits_a_reservation(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 3 * 4096, RW, 0);
    expect(uvm_release(&vm, base + 4096, 4096) == 0, "punching a hole succeeds");

    expect(uvm_fault_in(&vm, base, 1) == 1, "the left-hand side still faults in");
    expect(uvm_fault_in(&vm, base + 2 * 4096, 1) == 1,
           "the right-hand side still faults in");
    expect(uvm_fault_in(&vm, base + 4096, 1) == 0, "the punched page does not");
}

static void test_release_tolerates_holes(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 4096, RW, 0);
    expect(uvm_release(&vm, base, 2 * 4096) == 0,
           "releasing across an unreserved tail is not an error");
    expect(uvm_release(&vm, base, 4096) == 0, "releasing twice is not an error");
    expect(uvm_release(&vm, 0x800, 4096) == -1, "releasing an illegal range fails");
}

/* --- protect ----------------------------------------------------------- */

static void test_protect_commits_then_changes(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 2 * 4096, RW, 0);
    expect(uvm_protect(&vm, base, 2 * 4096, RO) == 0, "protecting the range succeeds");
    expect(frames_in_use == 2, "protect committed both lazy pages first");
    expect((vmm_entry_in(vm.user_pml4, base) & VMM_WRITE) == 0,
           "the first page lost its write bit");
    expect((vmm_entry_in(vm.user_pml4, base + 4096) & VMM_WRITE) == 0,
           "so did the second");
}

static void test_protect_changes_nothing_on_failure(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 2 * 4096, RW, 0);
    alloc_fail_after = 1; /* the first page commits, the second cannot */

    expect(uvm_protect(&vm, base, 2 * 4096, RO) == -1,
           "protect fails when a page cannot be committed");
    expect((vmm_entry_in(vm.user_pml4, base) & VMM_WRITE) != 0,
           "and no PTE in the range was re-permissioned");
}

static void test_protect_rejects_bad_ranges(void) {
    reset();
    u64 base = uvm_reserve(&vm, 0, 4096, RW, 0);
    expect(uvm_protect(&vm, base + 1, 4096, RO) == -1, "an unaligned address fails");
    expect(uvm_protect(&vm, base, 4095, RO) == -1, "an unrounded length fails");
    expect(uvm_protect(&vm, base, 4096, 0) == -1, "empty page flags fail");
    expect(uvm_protect(&vm, USER_STACK_LOW, 4096, RO) == -1,
           "protecting the stack range fails");
}

int main(void) {
    test_init_places_both_arenas();
    test_auto_placement_bumps();
    test_reserve_rejects_bad_ranges();
    test_fixed_rejects_occupied_range();
    test_reservation_table_is_bounded();

    test_fault_in_materialises_zeroed_page();
    test_write_to_readonly_reservation_is_refused();
    test_fault_outside_any_reservation();
    test_fault_in_releases_frame_when_mapping_fails();

    test_buffer_ok_spans_pages();
    test_buffer_ok_admits_the_stack();

    test_release_frees_frames_and_reuses_va();
    test_release_leaves_borrowed_frames_alone();
    test_holes_merge();
    test_release_splits_a_reservation();
    test_release_tolerates_holes();

    test_protect_commits_then_changes();
    test_protect_changes_nothing_on_failure();
    test_protect_rejects_bad_ranges();

    if (failures) {
        fprintf(stderr, "uvm_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("uvm_test: all checks passed\n");
    return 0;
}
