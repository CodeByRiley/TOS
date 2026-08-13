/* kernel/memory/vma.h — Virtual Memory Area allocator.
 *
 * Hands out unused kernel virtual address ranges.
 */
#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stddef.h>

/* The starting address for dynamic kernel virtual memory.
 * We place this after the typical kernel image location to avoid collisions. */
#define VMA_KERNEL_START 0xFFFFFFFFC0000000ULL

void vma_init(void);

/* Returns a virtual address of `size` bytes, page-aligned.
 * Does NOT map the memory. The caller must use vmm_map_in() afterwards. */
uint64_t vma_alloc(size_t size);

/* Allocate `size` bytes of kernel virtual memory and map physical pages to it.
 * This is the kernel equivalent of userspace mmap(). */
uint64_t vmalloc(size_t size);

/* Unmap the virtual range and free the physical frames */
void vfree(uint64_t virt, size_t size);
#endif
