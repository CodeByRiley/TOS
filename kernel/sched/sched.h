/* kernel/sched/sched.h — task table + scheduler surface.
 *
 * Holds:
 *   - struct task        — per-process kernel control block
 *   - enum task_state    — scheduler states
 *   - struct task_snap   — userspace-facing snapshot row (must match
 *                          userspace struct proc_info byte-for-byte;
 *                          static_asserts enforce the ABI)
 *   - task spawn / yield / block / exit / sleep helpers
 *
 * The scheduler is a single ready queue with PIT-driven preemption;
 * sleeping tasks live off-queue until sched_wake_sleepers re-queues
 * them.
 *
 * Implementation: kernel/sched/sched.c.
 */
#ifndef SCHED_H
#define SCHED_H

#define TASK_MAX_FDS 32
#define TASK_CWD_MAX 256
#include "msg/msg.h"
#include <stddef.h>
#include <stdint.h>

struct fat_file;

enum task_state {
  TASK_RUNNING,
  TASK_BLOCKED,
  TASK_ZOMBIE,    /* exited, awaiting parent reap                          */
  TASK_READY,
  TASK_DEAD,      /* slot free                                             */
  TASK_SLEEPING,  /* off the ready queue until PIT tick >= wake_tick       */
};

struct task {
  uint64_t saved_rsp;             /* kernel rsp at last context_switch     */
  uint64_t cr3;                   /* page-table root for this task         */
  uint64_t syscall_kstack_top;    /* SYSCALL entry stack (kernel_rsp_top)  */
  uint64_t user_rsp_saved;        /* user rsp at last syscall entry        */
  uint64_t user_entry;            /* used by user-task first-run trampoline*/
  uint64_t user_rsp_initial;      /* used by user-task first-run trampoline*/
  enum task_state state;
  int pid;
  int parent_pid;                 /* 0 for kthreads / init                 */
  int waiting_for_pid;            /* 0 unless TASK_BLOCKED on a child wait */
  int input_owner_restore_pid;     /* owner to restore after fb takeover    */
  /* Set at exit when no task was blocked waiting on this pid, meaning the
   * exit code will never be claimed and the slot can be freed by whoever
   * gets there first. Zombies without it belong to a waiting parent that
   * still has to read exit_code. */
  int unclaimed;
  long exit_code;
  void *kstack;                   /* base of allocation, for kfree on reap */
  void (*kthread_entry)(void);    /* entry for kthread tasks               */
  uint64_t *user_pml4;            /* owned PML4 for user tasks (else NULL) */
  struct task *next;
  uint64_t wake_tick;             /* PIT tick to wake at (TASK_SLEEPING)   */
  struct fat_file *fds[TASK_MAX_FDS];
  char cwd[TASK_CWD_MAX];					/* current working directory of the task ./bin etc */
  /* Per-task input ring (kbd/mouse events). Allocated on task spawn. */
  struct msg *input_ring;
  volatile int input_head;
  volatile int input_tail;

  /* Per-task IPC ring (cross-process control messages). */
  struct ipc_msg *ipc_ring;
  volatile int    ipc_head;
  volatile int    ipc_tail;

  /* Bump allocator for shmem regions mapped into this task by peers. */
  uint64_t shmem_next_va;
  uint64_t mmap_next_va;

  /* Pages this task has shared OUT to other tasks. Receivers carry
   * VMM_SHARED so their cleanup skips the frame, which makes the owner
   * solely responsible for it — so the owner's PML4 must not be freed
   * while any receiver still maps it. Nothing decrements this yet: there
   * is no unshare, and receiver exit does not notify the owner. See
   * task_reap_unclaimed. */
  int shmem_shared_out;

  /* x87 + SSE state saved/restored on context switch. 16-byte aligned per
   * fxsave's hardware contract. */
  uint8_t fxstate[512] __attribute__((aligned(16)));

  /* Human-readable task name (basename of ELF path for user tasks, hard-
   * coded for kthreads). Used by btop and other introspection consumers. */
  char     name[16];

  /* PIT-tick counter incremented from the timer IRQ when this task is the
   * one being preempted. Userspace btop computes CPU% from the delta over
   * a sampling window. Never reset by the kernel. */
  uint64_t ticks_run;
};

/* Snapshot row returned by sched_snapshot. Mirrors the userspace
 * struct proc_info in userspace/lib/syscall.h — keep both in sync. */
struct task_snap {
  uint64_t ticks_run;
  int      pid;
  int      parent_pid;
  int      state;          /* enum task_state, projected to int */
  char     name[16];
};

_Static_assert(sizeof(struct task_snap) == 40,
               "task_snap must match userspace proc_info size");
_Static_assert(offsetof(struct task_snap, ticks_run) == 0,
               "task_snap.ticks_run offset is userspace ABI");
_Static_assert(offsetof(struct task_snap, pid) == 8,
               "task_snap.pid offset is userspace ABI");
_Static_assert(offsetof(struct task_snap, name) == 20,
               "task_snap.name offset is userspace ABI");

/* Copy up to `max` live task rows into `out`. Returns number filled. Safe
 * to call from any context: walks the static task table, no allocations. */
int sched_snapshot(struct task_snap *out, int max);

/* Set a task's display name (truncated to 15 chars + nul). */
void task_set_name(struct task *t, const char *name);

void sched_init(void);

/* Spawn a kernel thread that runs `entry` until task_exit. */
struct task *task_spawn(void (*entry)(void));

/* Spawn a user-mode task. Caller has already prepared user_pml4 with the
 * ELF loaded and the user stack populated. `entry` and `user_rsp` are the
 * starting RIP/RSP for ring 3. Returns the task, queued ready. */
struct task *task_spawn_user(uint64_t *user_pml4, uint64_t entry,
                             uint64_t user_rsp, int parent_pid);

void task_yield(void);
void task_block(int waiting_for_pid);
void task_wakeup(struct task *t);
void task_exit(long code) __attribute__((noreturn));
int task_kill(int pid, long code);
struct task *task_current(void);

/* Park the current task off the ready queue until at least `ticks` PIT
 * ticks (100 Hz = 10 ms each) have elapsed. Called from PIT IRQ context
 * the kernel walks sleepers and re-queues any whose wake_tick has passed. */
void task_sleep_ticks(uint64_t ticks);
void sched_wake_sleepers(void);

/* Find a task by pid; returns 0 if none. Used by wait/reap. */
struct task *task_find(int pid);

/* Free a task that has reached TASK_ZOMBIE: kstack, owned PML4, slot. */
void task_reap(struct task *t);

/* Reap every zombie nobody is waiting on. Returns how many were freed.
 * Never touches a zombie whose waiter is still blocked on it, and never
 * the caller itself — reaping the running task would free the kstack it
 * is executing on. Called from the idle thread and on slot exhaustion. */
int task_reap_unclaimed(void);

#endif
