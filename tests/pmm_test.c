/* Host stress test for the production physical-frame bitmap allocator. */
#include "memory/pmm.h"
#include <stdint.h>
#include <stdio.h>

#define TEST_FRAMES 1024

static uint8_t bitmap[(TEST_FRAMES + 7) / 8];
static int failures;

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

static void reset_allocator(void) {
    pmm_test_reset(bitmap, TEST_FRAMES);
    pmm_test_mark_free(0x00010000, 0x00090000); /* ISA DMA pool */
    pmm_test_mark_free(0x00100000, 0x00280000); /* general pool */
}

static void test_general_allocation(void) {
    reset_allocator();
    uint64_t baseline = pmm_used_frames();
    uint64_t frames[300];

    for (int i = 0; i < 300; i++) {
        frames[i] = pmm_alloc_frame();
        expect(frames[i] >= 0x00100000, "general allocation stays above 1 MiB");
        if (i > 0)
            expect(frames[i] != frames[i - 1], "allocated frames are unique");
    }
    expect(pmm_used_frames() == baseline + 300,
           "used-frame counter tracks a large allocation run");

    for (int i = 0; i < 300; i += 2)
        pmm_free_frame(frames[i]);
    expect(pmm_used_frames() == baseline + 150,
           "used-frame counter tracks fragmented frees");

    for (int i = 0; i < 150; i++) {
        uint64_t frame = pmm_alloc_frame();
        expect(frame != 0, "fragmented frames are reusable");
    }
    expect(pmm_used_frames() == baseline + 300,
           "fragment refill restores the counter");
}

static void test_limits_and_contiguous(void) {
    reset_allocator();
    uint64_t baseline = pmm_used_frames();

    uint64_t low = pmm_alloc_frame_below(0x00100000);
    expect(low >= 0x00010000 && low < 0x000A0000,
           "sub-1 MiB request uses the ISA DMA pool");

    uint64_t general = pmm_alloc_frame_below(0x00400000);
    expect(general >= 0x00100000,
           "wide below-limit request preserves the ISA DMA pool");

    uint64_t run = pmm_alloc_contiguous_below(0x00400000, 64);
    expect(run >= 0x00100000, "large contiguous run comes from general RAM");
    expect((run & (FRAME_SIZE - 1)) == 0, "contiguous run is page aligned");
    expect(pmm_used_frames() == baseline + 66,
           "contiguous allocation charges every frame");

    pmm_free_contiguous(run, 64);
    expect(pmm_used_frames() == baseline + 2,
           "contiguous free releases every frame");
    expect(pmm_alloc_contiguous_below(0x00400000, 64) == run,
           "freed contiguous run is reusable");
    expect(pmm_alloc_contiguous_below(0x00400000, 0) == 0,
           "zero-length contiguous request is rejected");
}

static void test_exhaustion_and_invalid_free(void) {
    reset_allocator();
    uint64_t baseline = pmm_used_frames();
    uint64_t count = 0;
    while (pmm_alloc_frame() != 0)
        count++;

    expect(count == 640, "general allocator reaches every free general frame");
    expect(pmm_alloc_frame() == 0, "exhausted allocator stays exhausted");
    expect(pmm_used_frames() == baseline + count,
           "failed allocations do not change accounting");

    uint64_t used = pmm_used_frames();
    pmm_free_frame((TEST_FRAMES + 100) * FRAME_SIZE);
    expect(pmm_used_frames() == used, "out-of-range free is ignored");
    pmm_free_frame(0x00100000);
    pmm_free_frame(0x00100000);
    expect(pmm_used_frames() == used - 1, "double free cannot underflow accounting");
}

static void test_bitmap_boundaries(void) {
    /* Deliberately clear padding bits: they must never become real frames. */
    pmm_test_reset(bitmap, 259);
    pmm_test_mark_free(256 * FRAME_SIZE, 3 * FRAME_SIZE);
    bitmap[32] &= 0x07;
    for (uint64_t f = 256; f < 259; f++)
        expect(pmm_alloc_frame() == f * FRAME_SIZE,
               "partial final byte allocates only real frames in order");
    expect(pmm_alloc_frame() == 0, "padding bits are not physical frames");
    expect(pmm_used_frames() == 259, "padding does not affect accounting");

    pmm_test_reset(bitmap, 259);
    pmm_test_mark_free(257 * FRAME_SIZE, FRAME_SIZE);
    expect(pmm_alloc_frame_below(258 * FRAME_SIZE - 1) == 0,
           "limit excludes a frame that is not entirely below it");
    expect(pmm_alloc_frame_below(258 * FRAME_SIZE) == 257 * FRAME_SIZE,
           "limit admits the final whole frame in a partial byte");

    pmm_test_reset(bitmap, TEST_FRAMES);
    pmm_test_mark_free(300 * FRAME_SIZE, FRAME_SIZE);
    pmm_test_mark_free(305 * FRAME_SIZE, FRAME_SIZE);
    expect(pmm_alloc_frame() == 300 * FRAME_SIZE,
           "scan skips fully occupied bitmap bytes");
    /* The test setup frees memory without moving the next-fit cursor. */
    pmm_test_mark_free(299 * FRAME_SIZE, FRAME_SIZE);
    expect(pmm_alloc_frame() == 305 * FRAME_SIZE,
           "partial starting byte does not allocate below the cursor");
    expect(pmm_alloc_frame() == 299 * FRAME_SIZE,
           "wraparound finds a free bit below the original cursor");
    expect(pmm_alloc_frame() == 0, "wraparound does not reallocate taken bits");

    pmm_test_reset(bitmap, 255);
    pmm_test_mark_free(16 * FRAME_SIZE, FRAME_SIZE);
    expect(pmm_alloc_frame() == 0, "small memory map preserves the DMA pool");
    expect(pmm_alloc_frame_below(0x100000) == 16 * FRAME_SIZE,
           "small memory map still permits an explicit DMA allocation");
}

int main(void) {
    test_general_allocation();
    test_limits_and_contiguous();
    test_exhaustion_and_invalid_free();
    test_bitmap_boundaries();
    if (failures)
        return 1;
    puts("pmm_test: all checks passed");
    return 0;
}
