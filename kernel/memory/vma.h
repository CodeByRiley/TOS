/* kernel/memory/vma.h — Virtual Memory Area allocator.
 *
 * Hands out unused kernel virtual address ranges with metadata tracking.
 */
#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stddef.h>

/* The starting address for dynamic kernel virtual memory.
 * We place this after the typical kernel image location to avoid collisions. */
#define VMA_KERNEL_START 0xFFFFFFFFC0000000ULL

/* Structure tracking an individual VMA allocation block */
typedef struct vma_node {
    uint64_t virt_addr;
    size_t size;
    uint32_t ref_count;
    uint64_t last_used;       // System ticks or Epoch timestamp
    struct vma_node *next;
} vma_node_t;

/* Initialize the VMA allocator subsystem */
void vma_init(void);

/* Returns a virtual address range of `size` bytes, page-aligned.
 * Does NOT map physical memory. The caller must use vmm_map_in() afterwards. */
uint64_t vma_alloc(size_t size);

/* Allocate `size` bytes of kernel virtual memory and map physical pages to it. */
uint64_t vmalloc(size_t size);

/* Decrement reference count on allocation block.
 * Unmaps and frees physical pages only when ref_count reaches 0. */
void vfree(uint64_t virt);

/* Increment reference count on a shared virtual memory range. */
void vma_retain(uint64_t virt);

/* Update the last_used timestamp for an active VMA range (e.g., on touch/access). */
void vma_touch(uint64_t virt);

/* Look up VMA metadata node by virtual address (returns NULL if not found). */
vma_node_t *vma_find(uint64_t virt);

#endif /* VMA_H */
