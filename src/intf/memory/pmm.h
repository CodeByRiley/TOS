#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define FRAME_SIZE 4096ULL

void pmm_init(uint64_t mb2_addr);
uint64_t pmm_alloc_frame(void);
void pmm_free_frame(uint64_t frame);
uint64_t pmm_total_frames(void);
uint64_t pmm_usable_frames(void);
uint64_t pmm_used_frames(void);

#endif
