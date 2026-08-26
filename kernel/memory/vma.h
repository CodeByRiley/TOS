/* Kernel virtual-address allocator. */
#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stddef.h>

/* Dynamic kernel VA arena. */
#define VMA_KERNEL_START 0xFFFFFFFFC0000000ULL

typedef struct vma_node {
    uint64_t virt_addr;
    size_t size;
    uint32_t ref_count;
    uint64_t last_used;
    struct vma_node *next;
} vma_node_t;

void vma_init(void);

/* Reserve an unmapped, page-aligned VA range. */
uint64_t vma_alloc(size_t size);

/* Reserve a VA range and map physical pages. */
uint64_t vmalloc(size_t size);

/* Release a reference and free pages at zero. */
void vfree(uint64_t virt);

/* Retain a shared VA range. */
void vma_retain(uint64_t virt);

/* Refresh a range's last-use timestamp. */
void vma_touch(uint64_t virt);

/* Find range metadata by base address. */
vma_node_t *vma_find(uint64_t virt);

#endif /* VMA_H */
