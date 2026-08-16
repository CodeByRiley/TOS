/* kernel/sched/sched.c — task table + scheduler.
 *
 * Round-robin BSP ready queue. Tasks have explicit states
 * (RUNNING/READY/BLOCKED/SLEEPING/ZOMBIE/DEAD); sleeping tasks sit off the
 * ready queue and get re-queued by sched_wake_sleepers when their wake_tick
 * hits. Ring-3 code is preempted on a PIT time slice, while kernel code stays
 * cooperative. APs run the separate SMP-safe kernel work queue.
 *
 * Task lifecycle:
 *   - task_spawn         — kernel thread, runs `entry` until task_exit
 *   - task_spawn_user    — user task on top of a prepared PML4 + stack
 *   - task_exit          — sets ZOMBIE; waiter or reaper frees the slot
 *   - task_reap          — releases kstack, owned PML4, slot
 *
 * Per-task input + IPC rings, shmem bump allocator, and the FPU state
 * (fxsave area) all hang off struct task; the actual ring backends live
 * in msg/msg.c.
 *
 * Context switch: assembly in kernel/arch/x86_64/sched/switch.asm. Saves
 * callee-saved + rsp, swaps to the new task's saved_rsp.
 */

#include <sched/sched.h>
#include <devices/pit.h>
#include <display/framebuffer.h>
#include <drivers/sound/sb16.h>
#include <loader/process.h>
#include <memory/hhdm.h>
#include <arch/gdt.h>
#include <memory/heap.h>
#include <msg/msg.h>
#include <utilities/log.h>
#include <utilities/panic.h>
#include <utilities/string.h>
#include <stdint.h>

static void task_set_cwd_root(struct task *t) {
    if (!t) return;
    t->cwd[0] = '/';
    t->cwd[1] = 0;
}

void task_inherit_cwd(struct task *child, struct task *parent) {
    if (!child) return;
    if (!parent || !parent->cwd[0]) {
        task_set_cwd_root(child);
        return;
    }

    size_t i = 0;
    while (i < TASK_CWD_MAX - 1 && parent->cwd[i]) {
        child->cwd[i] = parent->cwd[i];
        i++;
    }
    child->cwd[i] = 0;
}

extern void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp,
                           uint64_t new_cr3,
                           void *old_fxstate, void *new_fxstate);

/* Capture the CPU's current x87/SSE state into `buf`. Used at task creation
 * so a fresh task's first context_switch fxrstors from a valid snapshot
 * instead of zeroed memory (which fxrstor would treat as a reserved-bits
 * fault). */
static void fxstate_init(void *buf) {
    __asm__ volatile ("fxsave (%0)" :: "r"(buf) : "memory");
}
extern uint64_t *kernel_pml4;

/* Globals owned by the SYSCALL entry stub. We re-stage these on every
 * context switch so that on the next user→kernel SYSCALL transition (or on
 * the next sysret return path) the correct per-task value is in place. */
extern uint64_t kernel_rsp_top;     /* syscall.asm */
extern uint64_t user_rsp_save;      /* syscall.asm */

/* Free a process PML4 — defined in loader/process.c. */
extern void free_user_pml4(uint64_t *pml4);

#define MSR_FS_BASE 0xC0000100u

static void sched_wrmsr(uint32_t msr, uint64_t value) {
  uint32_t lo = (uint32_t)value;
  uint32_t hi = (uint32_t)(value >> 32);
  __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static uint64_t sched_rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static struct task tasks[MAX_TASKS];
static int next_pid = 1;
static struct task *current = 0;
/* One round-robin queue per priority level; see SCHED_PRIO_* in sched.h. */
static struct task *ready_head[SCHED_PRIO_LEVELS] = { 0 };
static struct task *ready_tail[SCHED_PRIO_LEVELS] = { 0 };
#define SCHED_QUANTUM_MS 10U
static uint32_t slice_ticks = 0;
/* Count of tasks in TASK_SLEEPING. Maintained by task_sleep_ticks /
 * sched_wake_sleepers so the PIT IRQ can skip a full task-table walk on
 * every tick when nothing is asleep — which is the common case. */
static int n_sleeping = 0;


/* Allocate per-task message + IPC rings. Each task owns its own so the
 * kernel can route inputs to a single foreground process without
 * everyone sharing a global queue. */
static int alloc_rings_for(struct task *t) {
    t->input_ring = (struct msg*)kmalloc(sizeof(struct msg) * INPUT_RING_SIZE_LOCAL);
    if (!t->input_ring) return -1;
    memset(t->input_ring, 0, sizeof(struct msg) * INPUT_RING_SIZE_LOCAL);
    t->input_head = t->input_tail = 0;

    t->ipc_ring = (struct ipc_msg*)kmalloc(sizeof(struct ipc_msg) * IPC_RING_SIZE_LOCAL);
    if (!t->ipc_ring) {
        kfree(t->input_ring);
        t->input_ring = 0;
        return -1;
    }
    memset(t->ipc_ring, 0, sizeof(struct ipc_msg) * IPC_RING_SIZE_LOCAL);
    t->ipc_head = t->ipc_tail = 0;

    t->shmem_next_va = USER_SHMEM_BASE;
    t->mmap_next_va  = USER_MMAP_BASE;
    t->input_owner_restore_pid = -1;
    return 0;
}

static void free_rings_for(struct task *t) {
    if (t->input_ring) { kfree(t->input_ring); t->input_ring = 0; }
    if (t->ipc_ring)   { kfree(t->ipc_ring);   t->ipc_ring   = 0; }
}

/* Idle task lives outside the normal ready queue. ready_pop returns it as
 * a floor when the queue is empty so the CPU always has something to hlt
 * on. Never push it back via ready_push. */
static struct task *idle_task = 0;

static void user_task_trampoline(void);
static void idle_thread(void);
static void mark_task_exited(struct task *task, long code);

static uint64_t irq_save(void) {
  uint64_t rflags;
  __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
  return rflags;
}

static void irq_restore(uint64_t rflags) {
  if (rflags & (1ULL << 9))
    __asm__ volatile ("sti" ::: "memory");
}

/* Consecutive dispatches served from HIGH before a runnable NORMAL task is
 * guaranteed a turn. This is what keeps the scheduling weighted instead of
 * strict: winman never blocks — it yields at the bottom of its loop and is
 * immediately runnable again — so strict priority would hand it the CPU
 * forever and starve its own clients. At 4, a ready NORMAL task waits at
 * most 4 dispatches, while the display path still gets the large share of
 * wake-ups that keeps the desktop responsive. */
#define SCHED_HIGH_BURST 4

static int high_streak = 0;

static void ready_push(struct task *t) {
  int p = (t->prio == SCHED_PRIO_HIGH) ? SCHED_PRIO_HIGH : SCHED_PRIO_NORMAL;
  t->next = 0;
  if (!ready_tail[p])
    ready_head[p] = ready_tail[p] = t;
  else {
    ready_tail[p]->next = t;
    ready_tail[p] = t;
  }
}

/* Any runnable task at any level? Callers use this to decide whether
 * yielding would actually hand the CPU to someone. */
static int ready_any(void) {
  for (int p = 0; p < SCHED_PRIO_LEVELS; p++)
    if (ready_head[p])
      return 1;
  return 0;
}

static struct task *ready_pop(void) {
  int level = -1;

  if (ready_head[SCHED_PRIO_HIGH] &&
      (high_streak < SCHED_HIGH_BURST || !ready_head[SCHED_PRIO_NORMAL]))
    level = SCHED_PRIO_HIGH;
  else if (ready_head[SCHED_PRIO_NORMAL])
    level = SCHED_PRIO_NORMAL;
  else if (ready_head[SCHED_PRIO_HIGH])
    level = SCHED_PRIO_HIGH;

  if (level < 0) {
    /* Idle is a fallback, not a normal queue member. Returning it only when
     * another task blocks/exits keeps timer preemption from wasting a slice
     * on idle while real work is runnable. */
    high_streak = 0;
    if (idle_task && current != idle_task)
      return idle_task;
    return 0;
  }

  if (level == SCHED_PRIO_HIGH)
    high_streak++;
  else
    high_streak = 0;

  struct task *t = ready_head[level];
  ready_head[level] = t->next;
  if (!ready_head[level])
    ready_tail[level] = 0;
  t->next = 0;
  return t;
}

static int ready_remove(struct task *target) {
  for (int p = 0; p < SCHED_PRIO_LEVELS; p++) {
    struct task *previous = 0;
    for (struct task *task = ready_head[p]; task; task = task->next) {
      if (task != target) {
        previous = task;
        continue;
      }
      if (previous)
        previous->next = task->next;
      else
        ready_head[p] = task->next;
      if (ready_tail[p] == task)
        ready_tail[p] = previous;
      task->next = 0;
      return 0;
    }
  }
  return -1;
}

int sched_set_priority(struct task *t, int prio) {
  if (!t || (prio != SCHED_PRIO_NORMAL && prio != SCHED_PRIO_HIGH))
    return -1;

  uint64_t rflags = irq_save();
  if (t->prio != prio) {
    /* Only re-queue if it is actually on a ready list; a running, blocked
     * or sleeping task just picks the new level up next time it is pushed. */
    int queued = (ready_remove(t) == 0);
    t->prio = prio;
    if (queued)
      ready_push(t);
  }
  irq_restore(rflags);
  return 0;
}

static struct task *alloc_slot(void) {
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tasks[i].pid == 0)
      return &tasks[i];
  }

  /* Table full. Unclaimed zombies are holding slots nobody will ever come
   * back for — reclaim them and retry once. This is the path that makes
   * reaping correct under load: a system busy enough never to reach the
   * idle thread still frees exactly when it needs to. */
  if (task_reap_unclaimed() > 0) {
    for (int i = 0; i < MAX_TASKS; i++) {
      if (tasks[i].pid == 0)
        return &tasks[i];
    }
  }
  return 0;
}

/* Trampoline runs on first switch into a fresh kernel thread.
 * Reads entry from current task struct, calls it, exits with rc=0. */
static void kthread_trampoline(void) {
  __asm__ volatile ("sti" ::: "memory");
  void (*fn)(void) = current->kthread_entry;
  fn();
  task_exit(0);
}

static uint64_t kstack_aligned_top(void *kstack_base) {
  return ((uint64_t)kstack_base + KSTACK_BYTES) & ~0xFULL;
}

/* Build a kernel-stack frame matching context_switch's epilogue, which
 * pops r15, r14, r13, r12, rbp, rbx, ret. So we lay out (low → high):
 * [r15][r14][r13][r12][rbp][rbx][ret]. saved_rsp points at r15. */
static uint64_t build_initial_frame(void *kstack_base, void (*trampoline)(void)) {
  /* context_switch enters a fresh task with ret, not call. After that ret,
   * the trampoline still has to look like a normal SysV C callee: rsp % 16
   * must be 8 on function entry. */
  uint64_t *sp = (uint64_t *)(kstack_aligned_top(kstack_base) - 8);
  *--sp = (uint64_t)trampoline;     /* ret addr */
  *--sp = 0;                         /* rbx */
  *--sp = 0;                         /* rbp */
  *--sp = 0;                         /* r12 */
  *--sp = 0;                         /* r13 */
  *--sp = 0;                         /* r14 */
  *--sp = 0;                         /* r15 */
  return (uint64_t)sp;
}

void sched_init(void) {
  memset(tasks, 0, sizeof(tasks));
  /* Bootstrap: caller of sched_init becomes init task. No fake frame
   * needed — its rsp gets captured at the first context_switch. */
  struct task *t = alloc_slot();
  t->pid = next_pid++;
  t->state = TASK_RUNNING;
  t->cr3 = virt_to_phys(kernel_pml4);
  t->kstack = 0;
  t->kthread_entry = 0;
  t->user_pml4 = 0;
  t->pml4_ref_count = 0;
  /* The init task currently uses the boot SYSCALL kstack and the global
   * user_rsp_save scratch. After the very first context_switch out, both
   * will be re-staged for whatever task we switch to. */
  t->syscall_kstack_top = kernel_rsp_top;
  t->user_rsp_saved = 0;
  alloc_rings_for(t);
  task_set_cwd_root(t);
  fxstate_init(t->fxstate);
  task_set_name(t, "init");
  current = t;
  log_write("sched: init task pid=1 ready", KERNEL, LOG_INFO);

  /* Create idle through the normal spawn path, then detach it from the ready
   * queue. Its hlt loop runs only as ready_pop's fallback when nobody else
   * can run. */
  struct task *idle = task_spawn(idle_thread);
  if (idle) {
    task_set_name(idle, "idle");
    ready_remove(idle);
    idle_task = idle;
  }
  log_write("sched: idle task spawned", KERNEL, LOG_INFO);
}

struct task *task_current(void) { return current; }

int task_set_fs_base(uint64_t base) {
  struct task *t = task_current();
  if (!t)
    return -1;
  t->fs_base = base;
  sched_wrmsr(MSR_FS_BASE, base);
  return 0;
}

uint64_t task_get_fs_base(void) {
  struct task *t = task_current();
  return t ? t->fs_base : 0;
}

void task_set_name(struct task *t, const char *name) {
  if (!t) return;
  size_t i = 0;
  if (name) {
    while (i < sizeof(t->name) - 1 && name[i]) {
      t->name[i] = name[i];
      i++;
    }
  }
  t->name[i] = 0;
}

int sched_snapshot(struct task_snap *out, int max) {
  if (!out || max <= 0) return 0;
  int n = 0;
  for (int i = 0; i < MAX_TASKS && n < max; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0) continue;
    out[n].pid        = t->pid;
    out[n].parent_pid = t->parent_pid;
    out[n].state      = (int)t->state;
    out[n].ticks_run  = t->ticks_run;
    for (size_t k = 0; k < sizeof(out[n].name); k++) {
      out[n].name[k] = t->name[k];
    }
    n++;
  }
  return n;
}

struct task *task_find(int pid) {
  if (pid <= 0) return 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tasks[i].pid == pid) return &tasks[i];
  }
  return 0;
}

struct task *task_spawn(void (*entry)(void)) {
  struct task *t = alloc_slot();
  if (!t) {
    log_write("sched: task table full", KERNEL, LOG_ERROR);
    return 0;
  }

  void *stack_base = kmalloc(KSTACK_BYTES);
  if (!stack_base) {
    log_write("sched: kstack alloc failed", KERNEL, LOG_ERROR);
    return 0;
  }

  uint64_t stack_top = kstack_aligned_top(stack_base);
  t->saved_rsp = build_initial_frame(stack_base, kthread_trampoline);
  t->cr3 = virt_to_phys(kernel_pml4);
  t->syscall_kstack_top = stack_top;
  t->user_rsp_saved = 0;
  t->user_entry = 0;
  t->user_rsp_initial = 0;
  t->fs_base = 0;
  t->state = TASK_READY;
  t->prio = SCHED_PRIO_NORMAL;   /* callers raise it explicitly if needed */
  t->pid = next_pid++;
  t->parent_pid = 0;
  t->waiting_for_pid = 0;
  t->exit_code = 0;
  t->kstack = stack_base;
  t->kthread_entry = entry;
  t->user_pml4 = 0;
  t->pml4_ref_count = 0;
  t->next = 0;
  task_set_cwd_root(t);
  alloc_rings_for(t);
  fxstate_init(t->fxstate);

  ready_push(t);
  return t;
}

struct task *task_spawn_user(uint64_t *user_pml4, uint64_t entry,
                             uint64_t user_rsp, int parent_pid) {
  struct task *t = alloc_slot();
  if (!t) {
    log_write("sched: task table full", KERNEL, LOG_ERROR);
    return 0;
  }

  void *stack_base = kmalloc(KSTACK_BYTES);
  if (!stack_base) {
    log_write("sched: kstack alloc failed", KERNEL, LOG_ERROR);
    return 0;
  }

  uint64_t stack_top = kstack_aligned_top(stack_base);
  t->saved_rsp = build_initial_frame(stack_base, user_task_trampoline);
  t->cr3 = virt_to_phys(user_pml4);
  t->syscall_kstack_top = stack_top;
  t->user_rsp_saved = user_rsp;
  t->user_entry = entry;
  t->user_rsp_initial = user_rsp;
  t->fs_base = 0;
  t->state = TASK_READY;
  t->prio = SCHED_PRIO_NORMAL;
  t->pid = next_pid++;
  t->parent_pid = parent_pid;
  t->waiting_for_pid = 0;
  t->exit_code = 0;
  t->kstack = stack_base;
  t->kthread_entry = 0;
  t->user_pml4 = user_pml4;
  t->pml4_ref_count = (int *)kmalloc(sizeof(int));
  if (!t->pml4_ref_count) {
    kfree(stack_base);
    memset(t, 0, sizeof(*t));
    log_write("sched: pml4 refcount alloc failed", KERNEL, LOG_ERROR);
    return 0;
  }
  *t->pml4_ref_count = 1;
  t->next = 0;
  task_inherit_cwd(t, task_current());
  alloc_rings_for(t);
  fxstate_init(t->fxstate);

  ready_push(t);
  return t;
}

struct task *task_reserve_user(int parent_pid) {
  struct task *t = alloc_slot();
  if (!t) {
    log_write("sched: task table full", KERNEL, LOG_ERROR);
    return 0;
  }

  void *stack_base = kmalloc(KSTACK_BYTES);
  if (!stack_base) {
    log_write("sched: reserved user kstack alloc failed", KERNEL, LOG_ERROR);
    return 0;
  }

  memset(t, 0, sizeof(*t));
  uint64_t stack_top = kstack_aligned_top(stack_base);
  t->saved_rsp = build_initial_frame(stack_base, user_task_trampoline);
  t->cr3 = virt_to_phys(kernel_pml4);
  t->syscall_kstack_top = stack_top;
  t->state = TASK_LOADING;
  t->pid = next_pid++;
  t->parent_pid = parent_pid;
  t->kstack = stack_base;
  t->input_owner_restore_pid = -1;
  task_inherit_cwd(t, task_current());

  if (alloc_rings_for(t) != 0) {
    kfree(stack_base);
    memset(t, 0, sizeof(*t));
    log_write("sched: reserved user ring alloc failed", KERNEL, LOG_ERROR);
    return 0;
  }
  fxstate_init(t->fxstate);
  return t;
}

int task_activate_reserved_user(struct task *t, uint64_t *user_pml4,
                                uint64_t entry, uint64_t user_rsp) {
  if (!t || t->state != TASK_LOADING || !user_pml4 || !entry)
    return -1;

  int *refs = (int *)kmalloc(sizeof(int));
  if (!refs)
    return -1;
  *refs = 1;

  t->cr3 = virt_to_phys(user_pml4);
  t->user_pml4 = user_pml4;
  t->pml4_ref_count = refs;
  t->user_entry = entry;
  t->user_rsp_initial = user_rsp;
  t->user_rsp_saved = user_rsp;
  t->state = TASK_READY;
  ready_push(t);
  return 0;
}

void task_fail_reserved_user(struct task *t, long code) {
  if (!t || t->state != TASK_LOADING)
    return;
  mark_task_exited(t, code);
}

struct task *task_spawn_thread(uint64_t entry, uint64_t user_stack) {
    struct task *parent = task_current();
    if (!parent || !parent->user_pml4 || !parent->pml4_ref_count) return 0;

    struct task *t = alloc_slot();
    if (!t) {
        log_write("thread: task table full", KERNEL, LOG_ERROR);
        return 0;
    }

    void *stack_base = kmalloc(KSTACK_BYTES);
    if (!stack_base) {
        log_write("thread: kstack alloc failed", KERNEL, LOG_ERROR);
        return 0;
    }

    uint64_t stack_top = kstack_aligned_top(stack_base);
    t->saved_rsp = build_initial_frame(stack_base, user_task_trampoline);

    /* A thread shares its parent's address space rather than owning one, so
     * take a reference — whoever exits last frees the PML4. */
    t->cr3 = parent->cr3;
    t->user_pml4 = parent->user_pml4;
    t->pml4_ref_count = parent->pml4_ref_count;
    __atomic_add_fetch(t->pml4_ref_count, 1, __ATOMIC_ACQ_REL);

    /* Its kernel stack is its own, though: two threads sharing one would
     * corrupt each other the first time both entered a syscall. */
    t->syscall_kstack_top = stack_top;
    t->user_rsp_saved = user_stack;
    t->user_entry = entry;
    t->user_rsp_initial = user_stack;
    t->fs_base = parent->fs_base;

    t->state = TASK_READY;
    t->prio = SCHED_PRIO_NORMAL;
    t->pid = next_pid++;
    t->parent_pid = parent->pid;
    t->waiting_for_pid = 0;
    t->exit_code = 0;
    t->kstack = stack_base;
    t->kthread_entry = 0;
    t->next = 0;

    task_inherit_cwd(t, parent);
    alloc_rings_for(t);
    fxstate_init(t->fxstate);

    ready_push(t);
    return t;
}

/* Stage SYSCALL/ring-3 IRQ globals for `next` immediately before the
 * stack swap. These values are consumed by:
 *   - the SYSCALL stub (kernel_rsp_top): on the next user→kernel
 *     transition for `next`
 *   - syscall_entry's epilogue (user_rsp_save): if `next` was preempted
 *     mid-syscall, this is what gets popped into RSP just before sysret
 *   - any IRQ delivered while `next` runs in ring 3 (TSS RSP0)              */
static void stage_for(struct task *next) {
  kernel_rsp_top = next->syscall_kstack_top;
  user_rsp_save  = next->user_rsp_saved;
  sched_wrmsr(MSR_FS_BASE, next->fs_base);
  tss_set_rsp0(next->syscall_kstack_top);
}

/* Symmetric capture: park the global user-rsp scratch into `prev` so the
 * value survives across other tasks running. */
static void capture_from(struct task *prev) {
  prev->user_rsp_saved = user_rsp_save;
  prev->fs_base = sched_rdmsr(MSR_FS_BASE);
}

void task_yield(void) {
  uint64_t rflags = irq_save();
  slice_ticks = 0;
  struct task *next = ready_pop();
  if (!next) {
    irq_restore(rflags);
    return; /* nothing else runnable */
  }

  struct task *prev = current;
  prev->state = TASK_READY;
  if (prev != idle_task)
    ready_push(prev);

  capture_from(prev);

  next->state = TASK_RUNNING;
  current = next;
  stage_for(next);

  context_switch(&prev->saved_rsp, next->saved_rsp, next->cr3,
                 prev->fxstate, next->fxstate);
  irq_restore(rflags);
}

void sched_preempt_tick(void) {
  if (!current || current == idle_task || current->state != TASK_RUNNING) {
    slice_ticks = 0;
    return;
  }

  uint32_t frequency = pit_get_freq();
  uint32_t quantum = (frequency * SCHED_QUANTUM_MS + 999U) / 1000U;
  if (quantum == 0)
    quantum = 1;

  slice_ticks++;
  if (slice_ticks < quantum)
    return;
  slice_ticks = 0;

  /* The ready queues exclude idle, so an empty set means this task really
   * is the only runnable work and should retain the CPU. */
  if (ready_any())
    task_yield();
}

void task_block(int waiting_for_pid) {
  uint64_t rflags = irq_save();
  slice_ticks = 0;
  struct task *next = ready_pop();
  if (!next) {
    log_write("sched: blocked with no runnable task", KERNEL, LOG_ERROR);
    for (;;) __asm__ volatile ("cli; hlt");
  }

  struct task *prev = current;
  prev->state = TASK_BLOCKED;
  prev->waiting_for_pid = waiting_for_pid;
  /* Note: NOT pushed onto ready queue. Whoever satisfies the wait
   * (typically a child task_exit calling task_wakeup) will requeue us. */

  capture_from(prev);

  next->state = TASK_RUNNING;
  current = next;
  stage_for(next);

  context_switch(&prev->saved_rsp, next->saved_rsp, next->cr3,
                 prev->fxstate, next->fxstate);
  irq_restore(rflags);
}

void task_wakeup(struct task *t) {
  if (!t || t->state != TASK_BLOCKED) return;
  t->state = TASK_READY;
  t->waiting_for_pid = 0;
  ready_push(t);
}

int task_wake_futex(uint64_t phys) {
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0) continue;
    if (t->state == TASK_BLOCKED && t->futex_addr == phys) {
      t->futex_addr = 0;
      task_wakeup(t);
      return 1; // Woke 1 thread
    }
  }
  return 0;
}

void task_sleep_ticks(uint64_t ticks) {
  extern uint64_t pit_ticks(void);
  if (ticks == 0) return;

  uint64_t rflags = irq_save();
  slice_ticks = 0;
  current->wake_tick = pit_ticks() + ticks;
  current->state = TASK_SLEEPING;
  n_sleeping++;

  struct task *next = ready_pop();
  if (!next) {
    /* Nothing else runnable — fall back to halting until the next IRQ.
     * The PIT IRQ that does eventually fire will run sched_wake_sleepers
     * and put us back on the ready queue; we re-poll on the next loop. */
    current->state = TASK_RUNNING;
    n_sleeping--;
    irq_restore(rflags);
    while (pit_ticks() < current->wake_tick) __asm__ volatile ("hlt");
    return;
  }

  struct task *prev = current;
  capture_from(prev);

  next->state = TASK_RUNNING;
  current = next;
  stage_for(next);

  context_switch(&prev->saved_rsp, next->saved_rsp, next->cr3,
                 prev->fxstate, next->fxstate);
  irq_restore(rflags);
}

/* Called from the PIT IRQ. Short-circuits when n_sleeping == 0 (the common
 * case) so the IRQ handler isn't doing an MAX_TASKS-wide table walk on every
 * tick just to find nothing to do. */
void sched_wake_sleepers(void) {
  if (n_sleeping == 0) return;
  extern uint64_t pit_ticks(void);
  uint64_t now = pit_ticks();
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0) continue;
    if (t->state == TASK_SLEEPING && t->wake_tick <= now) {
      t->state = TASK_READY;
      n_sleeping--;
      ready_push(t);
    }
  }
}

/* Block the calling task for `ms`. Requires interrupts.
 *
 * The wakeup comes from sched_wake_sleepers() off IRQ0,
 * so with IF clear this task would sleep forever and take the kernel with it.
 * Callers running before the boot-time sti want sleep_ms_busy(). */
void sleep_ms(uint32_t ms) {
    REQUIRE_INTERRUPTS();

    if (ms == 0) {
        task_yield();
        return;
    }

    /* Round up: a sub-tick sleep must still yield at least one tick, or
     * sleep_ms(1) at 100 Hz would return immediately. */
    uint32_t freq = pit_get_freq();
    uint64_t ticks_to_sleep = ((uint64_t)ms * freq + 999) / 1000;

    task_sleep_ticks(ticks_to_sleep);
}

/* Spin for `ms` without yielding.
 *
 * Deliberately does NOT wait on pit_ticks(): callers are device bring-up
 * paths (USB/PCI controller resets) that run before the boot-time sti, where
 * IRQ0 never fires, the tick counter never advances, and a hlt loop hangs the
 * kernel outright. Channel 2 is polled, so it works with interrupts off. */
void sleep_ms_busy(uint32_t ms) {
    pit_delay_ms(ms);
}

/* Always-runnable lowest-priority task: hlts until the next IRQ, then
 * yields so any newly-ready task can run. Without this, task_yield would
 * return early when the ready queue is empty and the caller would spin.
 *
 * The `sti; hlt` pair is intentional and required: context_switch does NOT
 * save/restore RFLAGS, so when a user task in mid-syscall (IF=0) switches
 * to the idle thread, idle inherits IF=0. `hlt` with IF=0 ignores maskable
 * interrupts, which means PIT can never wake it and the system deadlocks.
 * The atomic `sti; hlt` sequence guarantees interrupts are enabled before
 * we halt. */
static void idle_thread(void) {
  for (;;) {
    /* Steady-state reaper. Reaching idle means no task is mid-syscall on a
     * kstack we might free, and the table walk is only paid when the system
     * has nothing better to do. alloc_slot covers the case where we never
     * get here. */
    task_reap_unclaimed();
    __asm__ volatile ("sti; hlt");
    task_yield();
  }
}

static void mark_task_exited(struct task *task, long code) {
  /* Audio is exclusive. A killed player must not leave DMA running or keep
   * the device locked away from the next process. */
  sb16_stream_release(task->pid);

  /* Drop cached backbuffer pages before this task's address space can be
   * reclaimed. Exact PID matching leaves registrations owned by sibling
   * threads sharing the same PML4 alone. */
  if (task->user_pml4)
    framebuffer_unregister_user(task->user_pml4, task->pid);

  /* A direct framebuffer process can temporarily take ownership from winman.
   * When it exits, hand ownership back to the saved owner if it still exists. */
  if (msg_input_owner() == task->pid) {
    int restore_pid = task->input_owner_restore_pid;
    struct task *restore = task_find(restore_pid);
    if (restore_pid > 0 && restore && restore->state != TASK_ZOMBIE) {
      msg_input_owner_force(restore_pid);
    } else {
      msg_input_owner_clear(task->pid);
    }
  }

  int wm_pid = msg_input_owner();
  if (wm_pid > 0 && wm_pid != task->pid) {
    struct ipc_msg note;
    memset(&note, 0, sizeof(note));
    note.type = IPC_PEER_EXITED;
    note.a = task->pid;
    ipc_send(wm_pid, &note, 0);
  }

  /* Dying while blocked on a child means that child's exit code will never
   * be read. Release it, or it sits as a claimed zombie forever with no
   * waiter left to claim it. Must run before we overwrite our own state. */
  if (task->state == TASK_BLOCKED && task->waiting_for_pid > 0) {
    struct task *awaited = task_find(task->waiting_for_pid);
    if (awaited && awaited->state == TASK_ZOMBIE)
      awaited->unclaimed = 1;
  }

  task->state = TASK_ZOMBIE;
  task->exit_code = code;

  /* Wake any task that was waiting on us via task_block(self.pid). O(MAX_TASKS)
   * because we don't have a wait-list per pid; with 16 slots this is cheaper
   * than maintaining a real data structure to be cheaper.
   *
   * The same sweep answers whether anyone can still claim our exit code. A
   * woken waiter reaps us itself; if nobody was waiting, nobody ever will
   * — process_spawn_async children and task_kill victims both land here —
   * so the slot is up for grabs. Without this the slot, the kstack and the
   * whole user PML4 leak, which at MAX_TASKS = 16 is a short road. */
  int claimed = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0) continue;
    if (t->state == TASK_BLOCKED && t->waiting_for_pid == task->pid) {
      task_wakeup(t);
      claimed = 1;
    }
  }
  task->unclaimed = !claimed;
}

int task_kill(int pid, long code) {
  struct task *target = task_find(pid);
  if (target && target->state == TASK_LOADING) {
    return process_cancel_async(pid, code);
  }
  /* Kernel threads and already-dead tasks are not killable from userspace. */
  if (!target || !target->user_pml4 || target->state == TASK_ZOMBIE
      || target->state == TASK_DEAD)
    return -1;

  if (target == current)
    task_exit(code);
  if (target->state == TASK_RUNNING)
    return -1;
  if (target->state == TASK_READY && ready_remove(target) != 0)
    return -1;
  if (target->state == TASK_SLEEPING && n_sleeping > 0)
    n_sleeping--;

  mark_task_exited(target, code);
  return 0;
}

void task_exit(long code) {
  irq_save();
  slice_ticks = 0;
  mark_task_exited(current, code);

  struct task *next = ready_pop();
  if (!next) {
    log_write("sched: last task exited, halting", KERNEL, LOG_ERROR);
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  struct task *prev = current;
  next->state = TASK_RUNNING;
  current = next;
  stage_for(next);

  /* We're still running on the dying task's kstack. context_switch will
   * stash our rsp into a stack local (overwritten anyway) and load next's.
   * Reap of kstack/PML4/slot happens later from task_reap, called by the
   * waiting parent once it wakes. */
  uint64_t throwaway;
  context_switch(&throwaway, next->saved_rsp, next->cr3,
                 prev->fxstate, next->fxstate);
  __builtin_unreachable();
}

void task_exit_thread(void) {
    irq_save();
    slice_ticks = 0;
    sb16_stream_release(current->pid);
    current->state = TASK_ZOMBIE;
    current->unclaimed = 1; /* nobody joins a bare thread; idle reaps it */

    /* Wake anything blocked in thread_join on our pid. */
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->pid == 0) continue;
        if (t->state == TASK_BLOCKED && t->waiting_for_pid == current->pid) {
            task_wakeup(t);
        }
    }

    struct task *next = ready_pop();
    if (!next) {
        log_write("sched: last thread exited, halting", KERNEL, LOG_ERROR);
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    struct task *prev = current;
    next->state = TASK_RUNNING;
    current = next;
    stage_for(next);

    uint64_t throwaway;
    context_switch(&throwaway, next->saved_rsp, next->cr3,
                   prev->fxstate, next->fxstate);
    __builtin_unreachable();
}

static void task_close_fds(struct task *t) {
    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (t->fds[i]) {
            kfree(t->fds[i]);
            t->fds[i] = 0;
        }
    }
}

static void task_release_address_space(struct task *t) {
  if (!t || !t->user_pml4)
    return;

  if (!t->pml4_ref_count) {
    free_user_pml4(t->user_pml4);
    t->user_pml4 = 0;
    return;
  }

  int *refs = t->pml4_ref_count;

  /* Atomic: sibling threads can reach this concurrently on different CPUs. */
  int new_count = __atomic_sub_fetch(refs, 1, __ATOMIC_ACQ_REL);
  if (new_count < 0) {
    log_write("sched: pml4 refcount underflow", KERNEL, LOG_ERROR);
  }

  if (new_count <= 0) {
    free_user_pml4(t->user_pml4);
    kfree(refs);
  }

  t->user_pml4 = 0;
  t->pml4_ref_count = 0;
}

void task_reap(struct task *t) {
  if (!t || t->state != TASK_ZOMBIE) return;

  task_close_fds(t);
  free_rings_for(t);

  if (t->kstack)    kfree(t->kstack);
  task_release_address_space(t);

  /* pid=0 marks the slot free for alloc_slot. */
  memset(t, 0, sizeof(*t));
}

int task_reap_unclaimed(void) {
  int reaped = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0 || t->state != TASK_ZOMBIE || !t->unclaimed)
      continue;
    /* task_exit runs on the dying task's kstack until it switches away, so
     * the current task is never safe to free from here. It cannot actually
     * be a zombie while running, but the guard costs nothing and documents
     * why this is not called from task_exit directly. */
    if (t == current)
      continue;
    /* Shared frames have exactly one owner: receivers carry VMM_SHARED so
     * their cleanup skips the frame (sys_shmem_share). Freeing this PML4
     * would hand those frames back to the PMM while the receiver is still
     * writing through them, and the next allocation would reuse them.
     * Leaking the slot is the lesser bug until shared frames are
     * refcounted; process_exec's explicit reap is unaffected. */
    if (t->shmem_shared_out > 0)
      continue;
    task_reap(t);
    reaped++;
  }
  return reaped;
}

static void user_task_trampoline(void) {
  uint64_t entry = current->user_entry;
  uint64_t rsp   = current->user_rsp_initial;
  uint64_t arg   = current->user_arg;

  __asm__ volatile (
      "cli                  \n"
      "mov $0x1B, %%ax      \n"      /* user data | RPL=3 */
      "mov %%ax, %%ds       \n"
      "mov %%ax, %%es       \n"
      "mov %%ax, %%fs       \n"
      "mov %%ax, %%gs       \n"
      "pushq $0x1B          \n"      /* SS */
      "pushq %1             \n"      /* user RSP */
      "pushq $0x202         \n"      /* RFLAGS */
      "pushq $0x23          \n"      /* user CS */
      "pushq %2             \n"      /* user RIP */
      "iretq                \n"
      :: "D"(arg), "r"(rsp), "r"(entry)  // "D" forces arg into RDI
      : "rax", "memory"
  );
  __builtin_unreachable();
}
