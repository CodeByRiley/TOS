#include "devices/pit.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include <memory/vma.h>

static vma_node_t *vma_head = NULL;
static u64 next_vma_addr = VMA_KERNEL_START;
extern u64 *kernel_pml4;

void vma_init(void) {
    next_vma_addr = VMA_KERNEL_START;
    vma_head = NULL;
}

vma_node_t *vma_find(u64 virt) {
    vma_node_t *curr = vma_head;
    while (curr) {
        if (curr->virt_addr == virt) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

u64 vma_alloc(usize size) {
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

u64 vmalloc(usize size) {
    u64 virt = vma_alloc(size);
    if (!virt) return 0;

    u64 num_pages = (size + 0xFFF) / 0x1000;
    u64 flags = VMM_PRESENT | VMM_WRITE | VMM_NX;

    for (u64 i = 0; i < num_pages; i++) {
        u64 phys = pmm_alloc_frame();
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

void vma_retain(u64 virt) {
    vma_node_t *node = vma_find(virt);
    if (node) {
        node->ref_count++;
        node->last_used = pit_ticks();
    }
}

void vma_touch(u64 virt) {
    vma_node_t *node = vma_find(virt);
    if (node) {
        node->last_used = pit_ticks();
    }
}

void vfree(u64 virt) {
    vma_node_t **curr = &vma_head;

    while (*curr) {
        if ((*curr)->virt_addr == virt) {
            vma_node_t *node = *curr;

            if (node->ref_count > 1) {
                node->ref_count--;
                node->last_used = pit_ticks();
                return;
            }

            u64 num_pages = (node->size + 0xFFF) / 0x1000;
            for (u64 i = 0; i < num_pages; i++) {
                u64 vaddr = virt + i * 0x1000;
                u64 phys = vmm_translate_in(kernel_pml4, vaddr);
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
