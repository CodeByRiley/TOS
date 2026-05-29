#include "syscall.h"
#include "../include/string.h"
#include <stdint.h>

#define PAGE_SIZE       4096ULL
#define HEAP_CHUNK_SIZE (64 * 1024ULL)

struct block {
    size_t size;
    int    free;
    struct block *next;
    struct block *prev;
};

static struct block *head = 0;
static struct block *tail = 0;

static size_t align16(size_t n) {
    return (n + 15) & ~15ULL;
}

static size_t align_page(size_t n) {
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int adjacent(struct block *a, struct block *b) {
    return (uint8_t*)a + sizeof(*a) + a->size == (uint8_t*)b;
}

static void append_block(struct block *b) {
    b->next = 0;
    b->prev = tail;
    if (tail) tail->next = b;
    else head = b;
    tail = b;
}

static struct block *request_block(size_t n) {
    size_t bytes = sizeof(struct block) + n;
    if (bytes < HEAP_CHUNK_SIZE) bytes = HEAP_CHUNK_SIZE;
    bytes = align_page(bytes);

    void *mem = mmap(bytes);
    if (!mem) return 0;

    struct block *b = (struct block*)mem;
    b->size = bytes - sizeof(*b);
    b->free = 1;
    append_block(b);
    return b;
}

static void split_block(struct block *b, size_t n) {
    if (b->size < n + sizeof(struct block) + 16) return;

    struct block *split = (struct block*)((uint8_t*)b + sizeof(*b) + n);
    split->size = b->size - n - sizeof(*b);
    split->free = 1;
    split->next = b->next;
    split->prev = b;
    if (b->next) b->next->prev = split;
    else tail = split;
    b->size = n;
    b->next = split;
}

void *malloc(size_t n) {
    if (n == 0) return 0;
    n = align16(n);

    struct block *b = head;
    while (b) {
        if (b->free && b->size >= n) {
            split_block(b, n);
            b->free = 0;
            return (uint8_t*)b + sizeof(struct block);
        }
        b = b->next;
    }

    b = request_block(n);
    if (!b) return 0;
    split_block(b, n);
    b->free = 0;
    return (uint8_t*)b + sizeof(struct block);
}

static void merge_next(struct block *b) {
    if (!b || !b->next || !b->next->free || !adjacent(b, b->next)) return;

    struct block *next = b->next;
    b->size += sizeof(*next) + next->size;
    b->next = next->next;
    if (b->next) b->next->prev = b;
    else tail = b;
}

void free(void *p) {
    if (!p) return;

    struct block *b = (struct block*)((uint8_t*)p - sizeof(struct block));
    b->free = 1;
    merge_next(b);
    if (b->prev && b->prev->free && adjacent(b->prev, b)) {
        merge_next(b->prev);
    }
}

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    if (sz && total / sz != n) return 0;

    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
