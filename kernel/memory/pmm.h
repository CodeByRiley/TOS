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
uint64_t pmm_alloc_frame_below(uint64_t limit);

void     pmm_free_frame(uint64_t frame);

/* Counters for diagnostics. */
uint64_t pmm_total_frames(void);
uint64_t pmm_usable_frames(void);
uint64_t pmm_used_frames(void);

#endif
