/* kernel/memory/uvm.h , the user address space.
 *
 * One struct task_vm is one user address space: a table of reservations
 * saying which ranges are legal and with what page permissions, an arena
 * for auto-placed mmap, and the rules for turning a reservation into real
 * frames. Mappings are anonymous and private; there is no file-backed
 * mapping, so a loader reserves a range and then reads section bytes into
 * it with read().
 *
 * Pages are lazy. uvm_reserve only records the range. Frames arrive on
 * first touch, from either the page-fault handler or from uvm_buffer_ok
 * when the kernel is about to read or write the range on the task's
 * behalf. Both routes go through uvm_fault_in, so both enforce the same
 * permissions: a write to a range reserved read-only is refused rather
 * than mapped, and the task takes one fault to its segfault instead of
 * spending a frame first.
 *
 * Every entry point takes the address space explicitly and none of them
 * consult the current task, so this module links on the host. The caller
 * keeps the address space alive across the call and holds whatever lock
 * protects it; nothing here sleeps.
 *
 * POSIX prot bits are the syscall layer's vocabulary, not this module's:
 * callers translate them to PTE flags (VMM_*) before crossing over.
 * Address-space layout constants live in loader/process.h.
 *
 * Implementation: kernel/memory/uvm.c.
 */
#ifndef UVM_H
#define UVM_H

#include <loader/process.h>
#include <stdint.h>
#include <utilities/types.h>

/* Reservation-table sizes. Exhausting either fails an mmap; neither is a
 * panic, and neither is visible to userspace. */
#define MAX_USER_VMAS 64
#define TASK_MMAP_HOLES 16

/* A freed arena range awaiting reuse. A zero len means the slot is free. */
struct vm_hole {
    u64 base;
    u64 len;
};

/* One reserved range. pte_flags are the flags its pages receive when they
 * are materialised, so they carry the range's permissions as well. */
struct user_vma {
    u64 start;
    u64 end;
    u64 pte_flags;
    u32 used;
};

/*
 * Address-space state.
 *
 * NULL for kernel-only tasks. May be shared by multiple threads if
 * threading is added later. The shmem fields are owned by the shared-memory
 * syscalls, not by this module.
 */
struct task_vm {
    u64 *user_pml4;
    int *pml4_ref_count;

    u64 shmem_next_va;
    u64 mmap_next_va;

    struct vm_hole mmap_holes[TASK_MMAP_HOLES];
    struct user_vma vmas[MAX_USER_VMAS];

    int shmem_shared_out;
};

/* Put a freshly allocated address space into its empty state: both arenas
 * at their bases, no reservations, no holes. Leaves user_pml4 and the
 * reference count alone, since the caller owns those. */
void uvm_init(struct task_vm *vm);

/* True if base .. base+bytes is a legal range to reserve: inside the user
 * half, clear of the stack, and free of wraparound. Ordinary syscall
 * buffers may live on the stack; a reservation may not, because it must
 * never allocate over it. */
int uvm_range_ok(u64 base, u64 bytes);

/* Reserve `bytes` and return the base address, or 0 on failure.
 *
 * With `fixed`, `addr` is honoured exactly and must be page-aligned, legal
 * and entirely unmapped. Without it `addr` is ignored and the range comes
 * from the mmap arena: first-fit over freed ranges, then a bump pointer.
 * `bytes` must already be page-rounded and non-zero. */
u64 uvm_reserve(struct task_vm *vm, u64 addr, u64 bytes, u64 pte_flags,
                int fixed);

/* Change the permissions of an already-reserved range. All-or-nothing: the
 * range is validated and every lazy page committed before a single PTE
 * changes, so a partial failure cannot leave a PE image half RX and half
 * RW. Returns 0, or -1 having changed nothing. */
int uvm_protect(struct task_vm *vm, u64 addr, u64 bytes, u64 pte_flags);

/* Drop a reservation and return its frames to the PMM, except borrowed
 * ones (VMM_SHARED), which are live in another address space. Ranges
 * inside the mmap arena go back on the free list; fixed ranges outside it
 * need no bookkeeping. Releasing a range with holes in it is not an error,
 * which is what POSIX says. Returns 0, or -1 if the range is illegal. */
int uvm_release(struct task_vm *vm, u64 addr, u64 bytes);

/* Materialise the zeroed page containing `addr` if a reservation covers it
 * and permits the access. Returns 1 when the page is present and usable
 * for that access afterwards, 0 otherwise , including when it is already
 * mapped but the access is not allowed. `addr` need not be page-aligned.
 *
 * This is the whole of user demand paging. The fault handler calls it to
 * service a not-present fault; uvm_buffer_ok calls it so that kernel
 * copy-in/copy-out never takes a nested supervisor-mode fault halfway
 * through a filesystem or display operation. */
int uvm_fault_in(struct task_vm *vm, u64 addr, int writable);

/* Validate a user buffer and materialise every page it covers. A
 * zero-length buffer passes. Returns 1 when the whole range is present and
 * usable for the access. */
int uvm_buffer_ok(struct task_vm *vm, const void *pointer, u64 bytes,
                  int writable);

#endif /* UVM_H */
