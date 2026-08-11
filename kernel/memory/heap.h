/* kernel/memory/heap.h — kernel heap (kmalloc/kfree).
 *
 * First-fit free list over pmm-backed pages. Initialised after pmm_init.
 * Userspace mmap is unrelated; this surface is kernel-internal only.
 *
 * Implementation: kernel/memory/heap.c.
 */
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

void  heap_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);

#endif
