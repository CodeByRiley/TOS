/* kernel/memory/heap.c — kernel heap (kmalloc/kfree).
 *
 * Doubly-linked first-fit allocator. The doubly-linked invariant means
 * kfree can coalesce in both directions in O(1), so the free list never
 * holds adjacent free blocks. That kills the need for a forward-sweep
 * pass inside kmalloc and bounds fragmentation tightly. Still a toy —
 * no buddy, no slab — but a well-behaved one.
 *
 * The heap lives at a high-canonical kernel VA (HEAP_BASE) and grows on
 * demand by paging in fresh frames from the PMM. heap_grow + list_append
 * keep the no-adjacent-free-blocks invariant intact across grows.
 */
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "utilities/string.h"
#include "utilities/log.h"
#include <stdint.h>

#define HEAP_BASE          0xFFFF900000000000ULL
#define HEAP_INITIAL_PAGES 256   /* 1 MiB */
#define SPLIT_THRESHOLD    16    /* payload bytes below which we won't split */

#define LARGE_ALLOC_VBASE 0xFFFFA00000000000ULL
static uint64_t large_alloc_virt_offset = LARGE_ALLOC_VBASE;

/* In-band header on every allocation. Doubly linked so kfree can fold
 * adjacent neighbours in O(1). */
struct block {
    size_t        size;          /* payload size, excluding header */
    int           free;
    struct block *next;
    struct block *prev;
};

static struct block *head = 0;
static struct block *tail = 0;
static uint64_t      heap_end = 0;

/* Map fresh frames at the top of the heap and report how many succeeded. */
static size_t heap_grow(size_t pages) {
    size_t mapped = 0;
    for (; mapped < pages; mapped++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            log_write("heap: OOM", KERNEL, LOG_ERROR);
            break;
        }
        if (vmm_map(heap_end, phys, VMM_PRESENT | VMM_WRITE) != 0) {
            pmm_free_frame(phys);
            log_write("heap: could not map growth page", KERNEL, LOG_ERROR);
            break;
        }
        heap_end += 4096;
    }
    return mapped;
}

void heap_init(void) {
    heap_end = HEAP_BASE;
    size_t initial_pages = heap_grow(HEAP_INITIAL_PAGES);
    if (initial_pages == 0) {
        log_write("heap: initial allocation failed", KERNEL, LOG_ERROR);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    head = (struct block*)HEAP_BASE;
    head->size = initial_pages * 4096 - sizeof(struct block);
    head->free = 1;
    head->next = 0;
    head->prev = 0;
    tail = head;
}

/* Append a freshly-grown region. If the existing tail is free, absorb
 * the new bytes into it instead of creating a new node — keeps the
 * no-adjacent-free-blocks invariant across heap_grow. */
static void list_append(uint64_t region_start, size_t region_bytes) {
    if (tail && tail->free) {
        tail->size += region_bytes;
        return;
    }
    struct block *nb = (struct block*)region_start;
    nb->size = region_bytes - sizeof(struct block);
    nb->free = 1;
    nb->next = 0;
    nb->prev = tail;
    if (tail) tail->next = nb;
    tail = nb;
    if (!head) head = nb;
}

void *kmalloc(size_t size) {
    if (size == 0) return 0;                       /* zero-byte: politely decline */
    size = (size + 7) & ~7ULL;                     /* 8-byte align */

    for (;;) {
        for (struct block *b = head; b; b = b->next) {
            if (!b->free || b->size < size) continue;

            /* Split only when the leftover would hold useful data,
             * otherwise hand back the whole thing as internal slack. */
            if (b->size >= size + sizeof(struct block) + SPLIT_THRESHOLD) {
                struct block *split = (struct block*)((uint8_t*)b + sizeof(struct block) + size);
                split->size = b->size - size - sizeof(struct block);
                split->free = 1;
                split->next = b->next;
                split->prev = b;
                if (b->next) b->next->prev = split;
                else          tail = split;
                b->size = size;
                b->next = split;
            }
            b->free = 0;
            return (uint8_t*)b + sizeof(struct block);
        }

        /* Out of fit. Grow heap, append (or merge into free tail), retry. */
        size_t needed = (size + sizeof(struct block) + 4095) / 4096;
        uint64_t old_end = heap_end;
        size_t mapped = heap_grow(needed);
        if (mapped == 0) return 0;
        list_append(old_end, mapped * 4096);
    }
}

void* large_alloc(size_t size) {
    if (size == 0) return 0;

    // Calculate how many 4KB pages we need
    size_t pages = (size + 4095) / 4096;

    // Record the starting virtual address to return later
    uint64_t virt_start = large_alloc_virt_offset;

    // Map physical frames to our virtual address range one by one
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            log_write("LARGE_ALLOC: PMM Out of Memory!", KERNEL, LOG_ERROR);
            return 0;
        }

        // Map the page with Present and Write flags
        if (vmm_map(virt_start + (i * 4096), phys, VMM_PRESENT | VMM_WRITE) != 0) {
            log_write("LARGE_ALLOC: VMM mapping failed!", KERNEL, LOG_ERROR);
            // Note: We leak the already allocated physical frames here for simplicity,
            // but in a production OS you would want to free them.
            return 0;
        }
    }

    // Advance the global offset for the next large allocation
    large_alloc_virt_offset += pages * 4096;

    return (void*)virt_start;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block *b = (struct block*)((uint8_t*)ptr - sizeof(struct block));
    b->free = 1;

    /* Forward-coalesce: absorb the next block if free. */
    if (b->next && b->next->free) {
        struct block *n = b->next;
        b->size += sizeof(struct block) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else          tail = b;
    }

    /* Backward-coalesce: absorb ourselves into the previous block if free. */
    if (b->prev && b->prev->free) {
        struct block *p = b->prev;
        p->size += sizeof(struct block) + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
        else          tail = p;
    }
}
