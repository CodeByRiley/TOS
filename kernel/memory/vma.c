#include "devices/pit.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include <memory/vma.h>

static vma_node_t *vma_head = NULL;
static uint64_t next_vma_addr = VMA_KERNEL_START;
extern uint64_t *kernel_pml4;

void vma_init(void) {
    next_vma_addr = VMA_KERNEL_START;
    vma_head = NULL;
}

vma_node_t *vma_find(uint64_t virt) {
    vma_node_t *curr = vma_head;
    while (curr) {
        if (curr->virt_addr == virt) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

uint64_t vma_alloc(size_t size) {
    if (size == 0) return 0;

    size = (size + 0xFFF) & ~0xFFF;

    vma_node_t *node = (vma_node_t *)kmalloc(sizeof(vma_node_t));
    if (!node) return 0;

    node->virt_addr = next_vma_addr;
    node->size = size;
    node->ref_count = 1;
    node->last_used = pit_ticks();
    node->next = vma_head;
    vma_head = node;

    next_vma_addr += size;
    return node->virt_addr;
}

uint64_t vmalloc(size_t size) {
    uint64_t virt = vma_alloc(size);
    if (!virt) return 0;

    uint64_t num_pages = (size + 0xFFF) / 0x1000;
    uint64_t flags = VMM_PRESENT | VMM_WRITE | VMM_NX;

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            log_write("VMALLOC: Out of physical memory!", KERNEL, LOG_ERROR);

            /* Release pages mapped before the allocation failed. */
            vfree(virt);
            return 0;
        }
        vmm_map_in(kernel_pml4, virt + i * 0x1000, phys, flags);
    }

    return virt;
}

void vma_retain(uint64_t virt) {
    vma_node_t *node = vma_find(virt);
    if (node) {
        node->ref_count++;
        node->last_used = pit_ticks();
    }
}

void vma_touch(uint64_t virt) {
    vma_node_t *node = vma_find(virt);
    if (node) {
        node->last_used = pit_ticks();
    }
}

void vfree(uint64_t virt) {
    vma_node_t **curr = &vma_head;

    while (*curr) {
        if ((*curr)->virt_addr == virt) {
            vma_node_t *node = *curr;

            if (node->ref_count > 1) {
                node->ref_count--;
                node->last_used = pit_ticks();
                return;
            }

            uint64_t num_pages = (node->size + 0xFFF) / 0x1000;
            for (uint64_t i = 0; i < num_pages; i++) {
                uint64_t vaddr = virt + i * 0x1000;
                uint64_t phys = vmm_translate_in(kernel_pml4, vaddr);
                if (phys) {
                    pmm_free_frame(phys);
                    vmm_unmap_in(kernel_pml4, vaddr);
                }
            }

            *curr = node->next;
            kfree(node);
            return;
        }
        curr = &(*curr)->next;
    }
}
