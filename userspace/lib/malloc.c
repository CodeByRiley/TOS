#include "syscall.h"
#include <stdint.h>

#define HEAP_SIZE (8 * 1024 * 1024)        // 8 MiB
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(16)));

struct block {
    size_t size;
    int    free;
    struct block *next;
};

static struct block *head = 0;

static void heap_init(void) {
    head = (struct block*)heap;
    head->size = HEAP_SIZE - sizeof(struct block);
    head->free = 1;
    head->next = 0;
}

void *malloc(size_t n) {
    if (!head) heap_init();
    n = (n + 15) & ~15ULL;
    struct block *b = head;
    while (b) {
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(struct block) + 16) {
                struct block *split = (struct block*)((uint8_t*)b + sizeof(struct block) + n);
                split->size = b->size - n - sizeof(struct block);
                split->free = 1;
                split->next = b->next;
                b->size = n;
                b->next = split;
            }
            b->free = 0;
            return (uint8_t*)b + sizeof(struct block);
        }
        b = b->next;
    }
    return 0;
}

void free(void *p) {
    if (!p) return;
    struct block *b = (struct block*)((uint8_t*)p - sizeof(struct block));
    b->free = 1;
    if (b->next && b->next->free) {
        b->size += sizeof(struct block) + b->next->size;
        b->next = b->next->next;
    }
}

void *calloc(size_t n, size_t sz) {
    void *p = malloc(n * sz);
    if (p) {
        uint8_t *q = (uint8_t*)p;
        for (size_t i = 0; i < n*sz; i++) q[i] = 0;
    }
    return p;
}
