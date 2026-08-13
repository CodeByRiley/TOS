#include "memory/pmm.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include <memory/vma.h>

static uint64_t next_vma_addr = VMA_KERNEL_START;
extern uint64_t *kernel_pml4;

void vma_init(void) {
    next_vma_addr = VMA_KERNEL_START;
}

uint64_t vma_alloc(size_t size) {
    if (size == 0) return 0;

    // Round size up to the nearest 4KB page boundary
    size = (size + 0xFFF) & ~0xFFF;

    uint64_t addr = next_vma_addr;
    next_vma_addr += size;
    return addr;
}

/* Allocate `size` bytes of kernel virtual memory and map physical pages to it.
 * This is the kernel equivalent of userspace mmap(). */
uint64_t vmalloc(size_t size) {
    uint64_t virt = vma_alloc(size);
    if (!virt) return 0;

    uint64_t num_pages = (size + 0xFFF) / 0x1000;
    uint64_t flags = VMM_PRESENT | VMM_WRITE | VMM_NX; // Kernel data is NX

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            // OOM! We should unmap and free previous pages here in a real OS.
            log_write("VMALLOC: Out of physical memory!", KERNEL, LOG_ERROR);
            return 0;
        }
        vmm_map_in(kernel_pml4, virt + i * 0x1000, phys, flags);
    }
    return virt;
}

/* Unmap the virtual range and free the physical frames */
void vfree(uint64_t virt, size_t size) {
    uint64_t num_pages = (size + 0xFFF) / 0x1000;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t vaddr = virt + i * 0x1000;
        uint64_t phys = vmm_translate_in(kernel_pml4, vaddr);
        if (phys) {
            pmm_free_frame(phys);
            vmm_unmap_in(kernel_pml4, vaddr);
        }
    }
}
