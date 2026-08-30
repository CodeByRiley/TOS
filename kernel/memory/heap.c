/* kernel/memory/heap.c , kernel heap (kmalloc/kfree).
 *
 * Doubly-linked first-fit allocator. The doubly-linked invariant means
 * kfree can coalesce in both directions in O(1), so the free list never
 * holds adjacent free blocks. That kills the need for a forward-sweep
 * pass inside kmalloc and bounds fragmentation tightly. Still a toy ,
 * no buddy, no slab , but a well-behaved one.
 *
 * The heap lives at a high-canonical kernel VA (HEAP_BASE) and grows on
 * demand by paging in fresh frames from the PMM. heap_grow + list_append
 * keep the no-adjacent-free-blocks invariant intact across grows.
 */
#include <memory/heap.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <utilities/string.h>
#include <utilities/log.h>
#include <stdint.h>

#define HEAP_BASE          0xFFFF900000000000ULL
#define HEAP_INITIAL_PAGES 256   /* 1 MiB */
#define SPLIT_THRESHOLD    16    /* payload bytes below which we won't split */

#define LARGE_ALLOC_VBASE 0xFFFFA00000000000ULL
static u64 large_alloc_virt_offset = LARGE_ALLOC_VBASE;

/* In-band header on every allocation. Doubly linked so kfree can fold
 * adjacent neighbours in O(1). */
struct block {
    usize        size;          /* payload size, excluding header */
    int           free;
    struct block *next;
    struct block *prev;
};

/* Payload alignment rests on three things: HEAP_BASE and every grown region
 * start being page-aligned, the header being a multiple of the alignment,
 * and kmalloc rounding every request up to it. Break any one and payloads
 * drift out of alignment. */
#define KMALLOC_ALIGN 16
_Static_assert(sizeof(struct block) % KMALLOC_ALIGN == 0,
               "block header must not break payload alignment");
_Static_assert(HEAP_BASE % KMALLOC_ALIGN == 0,
               "heap base must not break payload alignment");

static struct block *head = 0;
static struct block *tail = 0;
static u64      heap_end = 0;

/* Map fresh frames at the top of the heap and report how many succeeded. */
static usize heap_grow(usize pages) {
    usize mapped = 0;
    for (; mapped < pages; mapped++) {
        u64 phys = pmm_alloc_frame();
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
    usize initial_pages = heap_grow(HEAP_INITIAL_PAGES);
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
 * the new bytes into it instead of creating a new node , keeps the
 * no-adjacent-free-blocks invariant across heap_grow. */
static void list_append(u64 region_start, usize region_bytes) {
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

void *kmalloc(usize size) {
    if (size == 0) return 0;                       /* zero-byte: politely decline */
    /* 16, not 8: x86-64's widest scalar alignment, and the alignment fxsave
     * and fxrstor fault without. struct task_context carries an ALIGNED(16)
     * fxstate and is allocated from here, so an 8-byte-aligned payload would
     * fault on the first context switch into that task. */
    size = (size + (KMALLOC_ALIGN - 1)) & ~(usize)(KMALLOC_ALIGN - 1);

    for (;;) {
        for (struct block *b = head; b; b = b->next) {
            if (!b->free || b->size < size) continue;

            /* Split only when the leftover would hold useful data,
             * otherwise hand back the whole thing as internal slack. */
            if (b->size >= size + sizeof(struct block) + SPLIT_THRESHOLD) {
                struct block *split = (struct block*)((u8*)b + sizeof(struct block) + size);
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
            return (u8*)b + sizeof(struct block);
        }

        /* Out of fit. Grow heap, append (or merge into free tail), retry. */
        usize needed = (size + sizeof(struct block) + 4095) / 4096;
        u64 old_end = heap_end;
        usize mapped = heap_grow(needed);
        if (mapped == 0) return 0;
        list_append(old_end, mapped * 4096);
    }
}

/* Page-granular allocator for buffers too big to want a heap block, living in
 * its own VA range. The offset only ever moves forward and there is no
 * large_free, so every allocation is effectively permanent , fine for the
 * boot-time buffers using it, wrong for anything with a lifecycle. */
void* large_alloc(usize size) {
    if (size == 0) return 0;

    usize pages = (size + 4095) / 4096;
    u64 virt_start = large_alloc_virt_offset;

    for (usize i = 0; i < pages; i++) {
        u64 phys = pmm_alloc_frame();
        if (!phys) {
            log_write("LARGE_ALLOC: PMM Out of Memory!", KERNEL, LOG_ERROR);
            return 0;
        }

        /* Both failure paths leak every frame mapped so far, and leave the
         * partial mapping in place. Acceptable only because a failure here is
         * already fatal in practice. */
        if (vmm_map(virt_start + (i * 4096), phys, VMM_PRESENT | VMM_WRITE) != 0) {
            log_write("LARGE_ALLOC: VMM mapping failed!", KERNEL, LOG_ERROR);
            return 0;
        }
    }

    large_alloc_virt_offset += pages * 4096;
    return (void*)virt_start;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block *b = (struct block*)((u8*)ptr - sizeof(struct block));
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
