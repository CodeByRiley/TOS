/* Kernel virtual-address allocator. */
#ifndef VMA_H
#define VMA_H

#include <utilities/types.h>
#include <stdint.h>
#include <stddef.h>

/* Dynamic kernel VA arena. */
#define VMA_KERNEL_START 0xFFFFFFFFC0000000ULL

typedef struct vma_node {
    u64 virt_addr;
    usize size;
    u32 ref_count;
    u64 last_used;
    struct vma_node *next;
} vma_node_t;

void vma_init(void);

/* Reserve an unmapped, page-aligned VA range. */
u64 vma_alloc(usize size);

/* Reserve a VA range and map physical pages. */
u64 vmalloc(usize size);

/* Release a reference and free pages at zero. */
void vfree(u64 virt);

/* Retain a shared VA range. */
void vma_retain(u64 virt);

/* Refresh a range's last-use timestamp. */
void vma_touch(u64 virt);

/* Find range metadata by base address. */
vma_node_t *vma_find(u64 virt);

#endif /* VMA_H */
