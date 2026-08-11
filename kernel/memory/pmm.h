/* kernel/memory/pmm.h — physical-frame allocator.
 *
 * Bitmap-backed PMM. Frames are 4 KiB and identified by their physical
 * base address (always a multiple of FRAME_SIZE). Stats are exposed for
 * userspace SYS_MEM_STATS.
 *
 * Implementation: kernel/memory/pmm.c.
 */
#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define FRAME_SIZE 4096ULL

/* Parse the MB2 memory map and build the bitmap. */
void     pmm_init(uint64_t mb2_addr);

/* Allocate a frame, returning its physical base. 0 on out-of-memory. */
uint64_t pmm_alloc_frame(void);

/* Preserves the ISA DMA pool when limit is above 1 MiB. */
uint64_t pmm_alloc_frame_below(uint64_t limit);

/* Use for explicit low-memory/contiguous constraints such as ISA DMA. */
uint64_t pmm_alloc_contiguous_below(uint64_t limit, uint64_t num_frames);

void     pmm_free_frame(uint64_t frame);

/* Release a run obtained from pmm_alloc_contiguous_below. */
void     pmm_free_contiguous(uint64_t frame, uint64_t num_frames);

/* Counters for diagnostics. */
uint64_t pmm_total_frames(void);
uint64_t pmm_usable_frames(void);
uint64_t pmm_used_frames(void);

#endif
