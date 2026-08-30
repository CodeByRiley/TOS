/* kernel/memory/pmm.h , physical-frame allocator.
 *
 * Bitmap-backed PMM. Frames are 4 KiB and identified by their physical
 * base address (always a multiple of FRAME_SIZE). Stats are exposed for
 * userspace SYS_MEM_STATS.
 *
 * Implementation: kernel/memory/pmm.c.
 */
#ifndef PMM_H
#define PMM_H

#include <utilities/types.h>
#include <stdint.h>

#define FRAME_SIZE 4096ULL

/* Parse the MB2 memory map and build the bitmap. */
void     pmm_init(u64 mb2_addr);

/* Allocate a frame, returning its physical base. 0 on out-of-memory. */
u64 pmm_alloc_frame(void);

/* Preserves the ISA DMA pool when limit is above 1 MiB. */
u64 pmm_alloc_frame_below(u64 limit);

/* Use for explicit low-memory/contiguous constraints such as ISA DMA. */
u64 pmm_alloc_contiguous_below(u64 limit, u64 num_frames);

void     pmm_free_frame(u64 frame);

/* Release a run obtained from pmm_alloc_contiguous_below. */
void     pmm_free_contiguous(u64 frame, u64 num_frames);

/* Counters for diagnostics. */
u64 pmm_total_frames(void);
u64 pmm_usable_frames(void);
u64 pmm_used_frames(void);

#ifdef PMM_HOST_TEST
/* Test-only setup for executing the real bitmap allocator on the host. */
void pmm_test_reset(u8 *storage, u64 frame_count);
void pmm_test_mark_free(u64 base, u64 length);
#endif

#endif
