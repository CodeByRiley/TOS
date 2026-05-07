#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "utilities/string.h"
#include "utilities/log.h"
#include <stdint.h>

#define HEAP_BASE   0xFFFF800000000000ULL
#define HEAP_INITIAL_PAGES 16    // 64 KiB

struct block {
    size_t size;            // payload size, excluding header
    int    free;
    struct block *next;
};

static struct block *head = 0;
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
}

void *kmalloc(size_t size) {
    size = (size + 7) & ~7ULL;     // 8-byte align
    struct block *b = head;
    while (b) {
        if (b->free && b->size >= size) {
            // split if big enough
            if (b->size >= size + sizeof(struct block) + 16) {
                struct block *split = (struct block*)((uint8_t*)b + sizeof(struct block) + size);
                split->size = b->size - size - sizeof(struct block);
                split->free = 1;
                split->next = b->next;
                b->size = size;
                b->next = split;
            }
            b->free = 0;
            return (uint8_t*)b + sizeof(struct block);
        }
        b = b->next;
    }
    // grow heap
    size_t needed = (size + sizeof(struct block) + 4095) / 4096;
    uint64_t old_end = heap_end;
    heap_grow(needed);
    struct block *nb = (struct block*)old_end;
    nb->size = needed * 4096 - sizeof(struct block);
    nb->free = 1;
    nb->next = 0;
    // append
    struct block *t = head;
    while (t->next) t = t->next;
    t->next = nb;
    return kmalloc(size);          // retry
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block *b = (struct block*)((uint8_t*)ptr - sizeof(struct block));
    b->free = 1;
    // coalesce forward
    if (b->next && b->next->free) {
        b->size += sizeof(struct block) + b->next->size;
        b->next = b->next->next;
    }
}
