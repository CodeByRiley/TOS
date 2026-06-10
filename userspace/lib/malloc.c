/* userspace/lib/malloc.c — process-local heap allocator.
 *
 * First-fit free list over `mmap`-backed 64 KiB chunks. Blocks carry a
 * doubly-linked header (size / free flag / prev / next) and grow the heap
 * by requesting a fresh page-aligned region from the kernel when nothing
 * fits. free() coalesces with physically adjacent free neighbours.
 *
 * Trade-offs:
 *   - 16-byte alignment for all returned pointers.
 *   - No size classes, no slab — fragmentation is acceptable at the scale
 *     userspace apps currently run at (shell, btop, DOOM).
 *   - Not thread-safe (no userspace threads yet).
 */
#include "syscall.h"
#include "../include/string.h"
#include <stdint.h>

#define PAGE_SIZE       4096ULL
#define HEAP_CHUNK_SIZE (64 * 1024ULL)

/* In-band header preceding every allocation. */
struct block {
    size_t size;        /* payload bytes, not counting this header */
    struct block *next;
    struct block *prev;
    int    free;        /* 1 if available, 0 if handed out */
};

static struct block *head = 0;
static struct block *tail = 0;

/* Round up to a 16-byte boundary (alignment for returned pointers). */
static size_t align16(size_t n) {
    return (n + 15) & ~15ULL;
}

/* Round up to the next page boundary (mmap request granularity). */
static size_t align_page(size_t n) {
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/* True when `b` immediately follows `a` in memory (safe to merge). */
static int adjacent(struct block *a, struct block *b) {
    return (uint8_t*)a + sizeof(*a) + a->size == (uint8_t*)b;
}

/* Insert a fresh block at the tail of the free list. */
static void append_block(struct block *b) {
    b->next = 0;
    b->prev = tail;
    if (tail) tail->next = b;
    else head = b;
    tail = b;
}

/* Ask the kernel for a fresh chunk that can satisfy `n` bytes of payload. */
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

/* If `b` is much bigger than `n`, carve off the tail as a new free block. */
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

/* Standard malloc(3). Returns NULL on n==0 or out-of-memory. */
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

/* Merge `b` with its forward neighbour if both are free and adjacent. */
static void merge_next(struct block *b) {
    if (!b || !b->next || !b->next->free || !adjacent(b, b->next)) return;

    struct block *next = b->next;
    b->size += sizeof(*next) + next->size;
    b->next = next->next;
    if (b->next) b->next->prev = b;
    else tail = b;
}

/* Standard free(3). Coalesces with adjacent free neighbours. */
void free(void *p) {
    if (!p) return;

    struct block *b = (struct block*)((uint8_t*)p - sizeof(struct block));
    b->free = 1;
    merge_next(b);
    if (b->prev && b->prev->free && adjacent(b->prev, b)) {
        merge_next(b->prev);
    }
}

/* Standard calloc(3). Detects multiplication overflow before alloc. */
void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    if (sz && total / sz != n) return 0;

    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
