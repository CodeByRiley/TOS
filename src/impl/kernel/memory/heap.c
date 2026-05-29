#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "utilities/string.h"
#include "utilities/log.h"
#include <stdint.h>

#define HEAP_BASE   0xFFFF800000000000ULL
#define HEAP_INITIAL_PAGES 256   // 1 MiB
#define SPLIT_THRESHOLD    16    // payload bytes below which we won't bother splitting

/* Doubly-linked first-fit allocator. The doubly-linked invariant means
 * kfree can coalesce in both directions in O(1), so the free list never
 * contains adjacent free blocks. That kills the need for a forward-sweep
 * pass inside kmalloc and bounds fragmentation tightly. It's still a kernel
 * toy — no buddy, no slab — but at least it's a well-behaved toy now. */
struct block {
    size_t size;            // payload size, excluding header
    int    free;
    struct block *next;
    struct block *prev;
};

static struct block *head = 0;
static struct block *tail = 0;
static uint64_t      heap_end = 0;

static void heap_grow(size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) { log_write("heap: OOM", KERNEL, LOG_ERROR); return; }
        vmm_map(heap_end, phys, VMM_PRESENT | VMM_WRITE);
        heap_end += 4096;
    }
}

void heap_init(void) {
    heap_end = HEAP_BASE;
    heap_grow(HEAP_INITIAL_PAGES);
    head = (struct block*)HEAP_BASE;
    head->size = HEAP_INITIAL_PAGES * 4096 - sizeof(struct block);
    head->free = 1;
    head->next = 0;
    head->prev = 0;
    tail = head;
}

/* Append a freshly-grown region. If the existing tail is free, absorb the
 * new bytes into it instead of creating a new node — keeps the no-adjacent-
 * free-blocks invariant intact across heap_grow. */
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
    if (size == 0) return 0;                       // zero-byte alloc: politely decline
    size = (size + 7) & ~7ULL;                     // 8-byte align

    for (;;) {
        for (struct block *b = head; b; b = b->next) {
            if (!b->free || b->size < size) continue;

            /* Split only when the leftover would actually hold useful data,
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
        heap_grow(needed);
        if (heap_end == old_end) return 0;        // pmm_alloc_frame bailed mid-grow
        list_append(old_end, needed * 4096);
    }
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
