#include "page_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *page_aligned_alloc(size_t page_count, void **out_allocation) {
    if (out_allocation)
        *out_allocation = 0;
    if (page_count == 0)
        return 0;

    if (page_count > (SIZE_MAX - (USER_PAGE_SIZE - 1u)) / USER_PAGE_SIZE)
        return 0;

    size_t bytes = page_count * USER_PAGE_SIZE + (USER_PAGE_SIZE - 1u);
    void *allocation = malloc(bytes);
    if (!allocation)
        return 0;

    memset(allocation, 0, bytes);
    uintptr_t aligned = ((uintptr_t)allocation + USER_PAGE_SIZE - 1u) &
                        ~(uintptr_t)(USER_PAGE_SIZE - 1u);
    if (out_allocation)
        *out_allocation = allocation;
    return (void *)aligned;
}
