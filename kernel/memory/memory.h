/* kernel/memory/memory.h , umbrella header for the memory subsystem.
 *
 * Includes the physical (PMM), virtual (VMM), kernel heap, and
 * higher-half direct map (HHDM) headers. Callers can just include
 * this file to get access to all memory management utilities.
 */
#ifndef MEMORY_H
#define MEMORY_H

#include <memory/pmm.h>
#include <memory/vmm.h>
#include <memory/hhdm.h>
#include <memory/heap.h>
#include <memory/vma.h>

/* Standard page size on x86_64 */
#define PAGE_SIZE 4096

/* Helper macro to round values up to the nearest page boundary */
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* Helper macro to round values down to the nearest page boundary */
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))

#endif
