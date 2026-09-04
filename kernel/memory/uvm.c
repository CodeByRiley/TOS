/* kernel/memory/uvm.c , the user address space. See memory/uvm.h. */
#include <memory/uvm.h>

#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <utilities/log.h>
#include <utilities/string.h>

void uvm_init(struct task_vm *vm) {
  if (!vm)
    return;
  vm->shmem_next_va = USER_SHMEM_BASE;
  vm->mmap_next_va = USER_MMAP_BASE;
  vm->shmem_shared_out = 0;
  memset(vm->mmap_holes, 0, sizeof(vm->mmap_holes));
  memset(vm->vmas, 0, sizeof(vm->vmas));
}

int uvm_range_ok(u64 base, u64 bytes) {
  if (bytes == 0 || base < USER_VA_MIN)
    return 0;
  if (base + bytes < base || base + bytes > USER_VA_MAX)
    return 0;
  if (base < USER_STACK_TOP && base + bytes > USER_STACK_LOW)
    return 0;
  return 1;
}

static struct user_vma *vma_at(struct task_vm *vm, u64 address) {
  if (!vm)
    return 0;
  for (int i = 0; i < MAX_USER_VMAS; i++) {
    struct user_vma *vma = &vm->vmas[i];
    if (vma->used && address >= vma->start && address < vma->end)
      return vma;
  }
  return 0;
}

static struct user_vma *vma_free_slot(struct task_vm *vm) {
  if (!vm)
    return 0;
  for (int i = 0; i < MAX_USER_VMAS; i++) {
    if (!vm->vmas[i].used)
      return &vm->vmas[i];
  }
  return 0;
}

/* Every page in the range must be free for a fixed reservation to succeed. */
static int range_is_unmapped(struct task_vm *vm, u64 base, u64 bytes) {
  if (!vm)
    return 0;
  u64 end = base + bytes;
  for (int i = 0; i < MAX_USER_VMAS; i++) {
    struct user_vma *vma = &vm->vmas[i];
    if (vma->used && base < vma->end && end > vma->start)
      return 0;
  }
  for (u64 off = 0; off < bytes; off += 4096) {
    if (vmm_translate_in(vm->user_pml4, base + off))
      return 0;
  }
  return 1;
}

/* Record a freed arena range for reuse. Merge every overlapping or adjacent
 * hole, including duplicate releases, so arena_alloc can never return two
 * overlapping ranges from stale free-list entries. */
static void hole_add(struct task_vm *vm, u64 base, u64 bytes) {
  if (!vm)
    return;
  u64 end = base + bytes;
  for (int i = 0; i < TASK_MMAP_HOLES; i++) {
    struct vm_hole *h = &vm->mmap_holes[i];
    if (!h->len)
      continue;
    u64 hole_end = h->base + h->len;
    if (hole_end < base || h->base > end)
      continue;
    if (h->base < base)
      base = h->base;
    if (hole_end > end)
      end = hole_end;
    h->base = 0;
    h->len = 0;
    /* The expanded range may now touch a hole visited earlier. */
    i = -1;
  }

  for (int i = 0; i < TASK_MMAP_HOLES; i++) {
    if (!vm->mmap_holes[i].len) {
      vm->mmap_holes[i].base = base;
      vm->mmap_holes[i].len = end - base;
      return;
    }
  }
  /* List full: the range stays unmapped but its VA is not reused. */
  log_write("uvm: hole list full, leaking user VA", KERNEL, LOG_WARN);
}

/* First-fit over the free list, then the bump pointer. Returns 0 when the
 * arena is exhausted , never a valid address, since the arena starts well
 * above USER_VA_MIN. */
static u64 arena_alloc(struct task_vm *vm, u64 bytes) {
  if (!vm)
    return 0;
  for (int i = 0; i < TASK_MMAP_HOLES; i++) {
    struct vm_hole *h = &vm->mmap_holes[i];
    if (!h->len || h->len < bytes)
      continue;
    u64 base = h->base;
    h->base += bytes;
    h->len -= bytes;
    return base;
  }

  u64 base = vm->mmap_next_va;
  if (base + bytes < base || base + bytes > USER_MMAP_LIMIT)
    return 0;
  vm->mmap_next_va = base + bytes;
  return base;
}

/* Drop start..end from the reservation table. A hole wholly inside one
 * reservation needs one additional record for its right-hand side; reserve
 * that slot before changing anything so a release remains all-or-nothing
 * for metadata. */
static int vma_remove_range(struct task_vm *vm, u64 start, u64 end) {
  if (!vm)
    return -1;
  struct user_vma *split = 0;
  for (int i = 0; i < MAX_USER_VMAS; i++) {
    struct user_vma *vma = &vm->vmas[i];
    if (vma->used && start > vma->start && end < vma->end) {
      split = vma_free_slot(vm);
      if (!split)
        return -1;
      break;
    }
  }

  for (int i = 0; i < MAX_USER_VMAS; i++) {
    struct user_vma *vma = &vm->vmas[i];
    if (!vma->used || start >= vma->end || end <= vma->start)
      continue;

    if (start <= vma->start && end >= vma->end) {
      memset(vma, 0, sizeof(*vma));
    } else if (start <= vma->start) {
      vma->start = end;
    } else if (end >= vma->end) {
      vma->end = start;
    } else {
      *split = (struct user_vma){
          .start = end,
          .end = vma->end,
          .pte_flags = vma->pte_flags,
          .used = 1,
      };
      vma->end = start;
    }
  }
  return 0;
}

int uvm_fault_in(struct task_vm *vm, u64 addr, int writable) {
  if (!vm || !vm->user_pml4)
    return 0;

  u64 page = addr & ~4095ULL;
  u64 entry = vmm_entry_in(vm->user_pml4, page);
  if (entry) {
    return (entry & VMM_USER) && (!writable || (entry & VMM_WRITE));
  }

  struct user_vma *vma = vma_at(vm, page);
  if (!vma || !(vma->pte_flags & VMM_USER) ||
      (writable && !(vma->pte_flags & VMM_WRITE))) {
    return 0;
  }

  u64 phys = pmm_alloc_frame();
  if (!phys)
    return 0;
  memset((void *)phys_to_virt(phys), 0, 4096);
  if (vmm_map_in(vm->user_pml4, page, phys, vma->pte_flags) != 0) {
    pmm_free_frame(phys);
    return 0;
  }
  return 1;
}

int uvm_buffer_ok(struct task_vm *vm, const void *pointer, u64 bytes,
                  int writable) {
  if (bytes == 0)
    return 1;
  if (!vm || !vm->user_pml4)
    return 0;

  u64 base = (u64)(uintptr_t)pointer;
  u64 end = base + bytes;
  if (base < USER_VA_MIN || end < base)
    return 0;

  /* uvm_range_ok intentionally rejects the stack because a reservation
   * must never allocate over it; ordinary syscall buffers may still live
   * there, so the stack is admitted explicitly here. */
  int in_general_range = end <= USER_VA_MAX;
  int in_stack = base >= USER_STACK_LOW && end <= USER_STACK_TOP;
  if (!in_general_range && !in_stack)
    return 0;

  u64 page = base & ~4095ULL;
  u64 last = (end - 1) & ~4095ULL;
  for (;;) {
    if (!uvm_fault_in(vm, page, writable))
      return 0;
    if (page == last)
      break;
    page += 4096;
  }
  return 1;
}

u64 uvm_reserve(struct task_vm *vm, u64 addr, u64 bytes, u64 pte_flags,
                int fixed) {
  if (!vm || !vm->user_pml4 || !bytes || (bytes & 4095) || !pte_flags)
    return 0;

  struct user_vma *vma = vma_free_slot(vm);
  if (!vma)
    return 0;

  u64 base;
  if (fixed) {
    if (addr & 4095)
      return 0;
    if (!uvm_range_ok(addr, bytes))
      return 0;
    if (!range_is_unmapped(vm, addr, bytes))
      return 0;
    base = addr;
  } else {
    base = arena_alloc(vm, bytes);
    if (!base)
      return 0;
  }

  *vma = (struct user_vma){
      .start = base,
      .end = base + bytes,
      .pte_flags = pte_flags,
      .used = 1,
  };
  return base;
}

int uvm_protect(struct task_vm *vm, u64 addr, u64 bytes, u64 pte_flags) {
  if (!vm || !vm->user_pml4 || (addr & 4095) || (bytes & 4095) || !pte_flags)
    return -1;
  if (!uvm_range_ok(addr, bytes))
    return -1;

  /* Commit every lazy page first. After this each page has a PTE whose
   * permissions can be changed without consulting the reservation. */
  for (u64 off = 0; off < bytes; off += 4096) {
    if (!uvm_fault_in(vm, addr + off, 0))
      return -1;
  }

  for (u64 off = 0; off < bytes; off += 4096) {
    if (vmm_protect_in(vm->user_pml4, addr + off, pte_flags) != 0)
      return -1;
  }
  return 0;
}

int uvm_release(struct task_vm *vm, u64 addr, u64 bytes) {
  if (!vm || !vm->user_pml4 || (addr & 4095) || (bytes & 4095))
    return -1;
  if (!uvm_range_ok(addr, bytes))
    return -1;

  if (vma_remove_range(vm, addr, addr + bytes) != 0)
    return -1;

  for (u64 off = 0; off < bytes; off += 4096) {
    u64 va = addr + off;
    u64 entry = vmm_entry_in(vm->user_pml4, va);
    if (!entry)
      continue;
    vmm_unmap_in(vm->user_pml4, va);
    /* Mask to bits 12-51: VMM_NX lives at bit 63, so stripping the low
     * flag bits alone would hand the PMM an address with it still set. */
    if (!(entry & VMM_SHARED))
      pmm_free_frame(entry & VMM_ADDR_MASK);
  }

  if (addr >= USER_MMAP_BASE && addr + bytes <= USER_MMAP_LIMIT)
    hole_add(vm, addr, bytes);
  return 0;
}
