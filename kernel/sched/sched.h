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
 * The BSP scheduler uses one round-robin ready queue per priority level.
 * Tasks switch at yield/block/sleep points, and user-mode execution is
 * preempted on a short PIT-driven time slice. Kernel-mode execution remains
 * non-preemptible.
 *
 * Implementation: kernel/sched/sched.c.
 */
#ifndef SCHED_H
#define SCHED_H

/* Priority levels. NORMAL is 0 so a zeroed task slot defaults to it — no
 * path can accidentally inherit HIGH by forgetting to initialise.
 *
 * HIGH exists for the display critical path only: the window manager and
 * the framebuffer flush thread. Every client's pixels reach the screen
 * through those two, so leaving them to compete round-robin with their own
 * clients means one busy app decides the whole desktop's frame rate.
 * Scheduling is weighted, not strict — see SCHED_HIGH_BURST — so a spinning
 * HIGH task slows the system down instead of wedging it. */
#define SCHED_PRIO_NORMAL 0
#define SCHED_PRIO_HIGH   1
#define SCHED_PRIO_LEVELS 2

#define TASK_MAX_FDS 32
#define TASK_CWD_MAX 256

/* Arena bases come from loader/process.h — see the user address-space map. */
#define INPUT_RING_SIZE_LOCAL 64
#define IPC_RING_SIZE_LOCAL   16

/* BSP scheduler with a fixed 16-slot task table and a singly-linked ready
 * queue. APs service the separate SMP-safe kernel work queue; userspace stays
 * on the BSP until scheduler state is made per-CPU. */

#define MAX_TASKS 16
#define KSTACK_BYTES (16 * 1024)



/* Freed mmap ranges, kept for reuse. Small and fixed: the arena is a bump
 * allocator, and this list is what stops a load/unload cycle from walking
 * mmap_next_va to the top of the arena and never coming back. If it fills
 * up, the oldest hole is dropped — that VA is leaked for the life of the
 * process, exactly as it was before the list existed. */
#define TASK_MMAP_HOLES 16

#include <msg/msg.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_USER_VMAS 64

struct fat_file;

struct vm_hole {
  uint64_t base;
  uint64_t len;                   /* 0 marks an unused slot                */
};

enum task_state {
  TASK_RUNNING,
  TASK_BLOCKED,
  TASK_ZOMBIE,    /* exited, awaiting parent reap                          */
  TASK_READY,
  TASK_DEAD,      /* slot free                                             */
  TASK_SLEEPING,  /* off the ready queue until PIT tick >= wake_tick       */
  TASK_LOADING,   /* stable pid reserved while its image is loaded         */
};

struct user_vma {
    uint64_t start;
    uint64_t end;
    uint64_t pte_flags; // The VMM_* flags (USER, WRITE, NX, etc.)
    int used;
};

struct task {
  uint64_t saved_rsp;             /* kernel rsp at last context_switch     */
  uint64_t cr3;                   /* page-table root for this task         */
  uint64_t syscall_kstack_top;    /* SYSCALL entry stack (kernel_rsp_top)  */
  uint64_t user_rsp_saved;        /* user rsp at last syscall entry        */
  uint64_t user_entry;            /* used by user-task first-run trampoline*/
  uint64_t user_rsp_initial;      /* used by user-task first-run trampoline*/
  uint64_t user_arg;                 /* argument for user task                */

  enum task_state state;
  int prio;                       /* SCHED_PRIO_*; 0 (NORMAL) by default   */
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
  int *pml4_ref_count;         /* shared user_pml4 lifetime counter      */
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

  uint64_t futex_addr; 					  /* physical address of the futex lock for this task */

  /* Bump allocator for shmem regions mapped into this task by peers. */
  uint64_t shmem_next_va;
  uint64_t mmap_next_va;

  /* Ranges returned to the mmap arena by munmap, reusable by the next
   * auto-placed mmap. MAP_FIXED mappings outside the arena are not
   * tracked here: their addresses come from the caller, so a collision
   * check against the page tables is the only bookkeeping they need. */
  struct vm_hole mmap_holes[TASK_MMAP_HOLES];

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
   * running BSP task. Userspace btop computes CPU% from the delta over
   * a sampling window. Never reset by the kernel. */
  uint64_t ticks_run;
  /* Userspace Virtual Memory Areas for demand paging */
  struct user_vma vmas[MAX_USER_VMAS];
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

void sleep_ms(uint32_t ms);
void sleep_ms_busy(uint32_t ms);

/* Spawn a kernel thread that runs `entry` until task_exit. */
struct task *task_spawn(void (*entry)(void));

/* Spawn a user-mode task. Caller has already prepared user_pml4 with the
 * ELF loaded and the user stack populated. `entry` and `user_rsp` are the
 * starting RIP/RSP for ring 3. Returns the task, queued ready. */
struct task *task_spawn_user(uint64_t *user_pml4, uint64_t entry,
                             uint64_t user_rsp, int parent_pid);

/* Two-phase user spawn used by process_spawn_async. The reservation owns its
 * final pid, kernel stack, rings and inherited cwd, but is not runnable until
 * task_activate_reserved_user publishes the completed address space. */
struct task *task_reserve_user(int parent_pid);
int task_activate_reserved_user(struct task *t, uint64_t *user_pml4,
                                uint64_t entry, uint64_t user_rsp);
void task_fail_reserved_user(struct task *t, long code);

struct task *task_spawn_thread(uint64_t entry, uint64_t user_stack);

void task_yield(void);

/* Called after IRQ0 has been acknowledged when its interrupt frame came from
 * ring 3. Rotates runnable user tasks once their time slice expires. */
void sched_preempt_tick(void);

void task_block(int waiting_for_pid);
void task_wakeup(struct task *t);
int task_wake_futex(uint64_t phys);
void task_exit(long code) __attribute__((noreturn));
void task_exit_thread(void) __attribute__((noreturn));
int task_kill(int pid, long code);
struct task *task_current(void);

/* Move `t` to `prio`. Safe on a queued task: it is lifted off its current
 * ready list and re-queued on the new one. Returns 0, or -1 on a bad
 * argument. */
int sched_set_priority(struct task *t, int prio);

void task_inherit_cwd(struct task *child, struct task *parent);

/* Park the current task off the ready queue until at least `ticks` PIT
 * ticks have elapsed. From IRQ0, the kernel walks sleepers and re-queues any
 * whose wake_tick has passed. */
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
