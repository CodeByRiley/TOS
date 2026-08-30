/* Heap-backed page-aligned storage for userspace shared-memory buffers. */
#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stddef.h>

#define USER_PAGE_SIZE 4096u

/* Allocate and zero `page_count` contiguous pages. The aligned address is
 * returned to the caller and the original heap allocation is written to
 * `out_allocation`; free that original pointer when finished. */
void *page_aligned_alloc(size_t page_count, void **out_allocation);

#endif
