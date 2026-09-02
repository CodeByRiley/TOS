/* FIFO sleeping gate for the current BSP-only, cooperative kernel scheduler.
 * IRQ exclusion protects ONLY the queue, never filesystem or disk operations.
 * AP/IRQ callers must dispatch filesystem work to a BSP task instead. */
#include "lock.h"
#include <arch/percpu.h>
#include <interrupts/idt.h>
#include <sched/sched.h>
#include <utilities/panic.h>

#ifdef VFS_HOST_TEST
/* Tests replace CPU interrupt exclusion and scheduler parking, not this queue. */
u64 vfs_test_irq_save(void);
void vfs_test_irq_restore(u64 flags);
#define gate_irq_save vfs_test_irq_save
#define gate_irq_restore vfs_test_irq_restore
#else
static u64 gate_irq_save(void) {
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static void gate_irq_restore(u64 flags) {
    if (flags & (1ull << 9)) __asm__ volatile("sti" ::: "memory");
}
#endif

struct vfs_waiter {
    struct task *task;
    struct vfs_waiter *next;
};
static struct task boot_owner;
static struct task *owner;
static struct vfs_waiter *head, *tail;

static struct task *caller(void) {
    if (percpu_current_id() != 0 || irq_in_handler())
        panic("VFS requires BSP task context, not an AP or IRQ");
    struct task *task = task_current();
    return task ? task : &boot_owner;
}

void vfs_lock(void) {
    struct task *task = caller();
    u64 flags = gate_irq_save();
    if (owner == task || task->vfs_active)
        panic("recursive VFS entry; use an already-locked helper");
    /* Covers waiters too: task_kill must not reclaim a queued task's stack,
     * descriptor table, or user buffer before this operation returns. */
    task->vfs_active = 1;
    if (!owner) {
        owner = task;
    } else {
        if (task == &boot_owner) panic("contended VFS during bootstrap");
        struct vfs_waiter waiter = { .task = task };
        if (tail) tail->next = &waiter;
        else head = &waiter;
        tail = &waiter;
        /* BSP-only: enqueue + block cannot lose a wakeup while IRQs are off.
         * -1 is not a process PID, so process exit cannot wake this waiter. */
        do { task_block(-1); } while (owner != task);
    }
    gate_irq_restore(flags);
}

void vfs_unlock(void) {
    struct task *task = caller();
    u64 flags = gate_irq_save();
    if (owner != task || !task->vfs_active) panic("VFS unlock by non-owner");
    task->vfs_active = 0;
    if (head) {
        struct vfs_waiter *next = head;
        head = next->next;
        if (!head) tail = 0;
        /* Direct handoff prevents a new caller from overtaking queued work. */
        owner = next->task;
        task_wakeup(owner);
    } else owner = 0;
    gate_irq_restore(flags);
}

void vfs_assert_locked(void) {
    struct task *task = caller();
    if (owner != task || !task->vfs_active)
        panic("VFS backend/helper called outside serialization boundary");
}
