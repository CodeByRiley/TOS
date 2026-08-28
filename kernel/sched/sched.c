/* kernel/sched/sched.c , task table + scheduler.
 *
 * Round-robin BSP ready queue. Tasks have explicit states
 * (RUNNING/READY/BLOCKED/SLEEPING/ZOMBIE/DEAD); sleeping tasks sit off the
 * ready queue and get re-queued by sched_wake_sleepers when their wake_tick
 * hits. Ring-3 code is preempted on a PIT time slice, while kernel code stays
 * cooperative. APs run the separate SMP-safe kernel work queue.
 *
 * Task lifecycle:
 *   - task_spawn         , kernel thread, runs `entry` until task_exit
 *   - task_spawn_user    , user task on top of a prepared PML4 + stack
 *   - task_exit          , sets ZOMBIE; waiter or reaper frees the slot
 *   - task_reap          , releases kstack, owned PML4, slot
 *
 * Per-task input + IPC rings, shmem bump allocator, and the FPU state
 * (fxsave area) all hang off struct task; the actual ring backends live
 * in msg/msg.c.
 *
 * Context switch: assembly in kernel/arch/x86_64/sched/switch.asm. Saves
 * callee-saved + rsp, swaps to the new task's saved_rsp.
 */

#include <arch/gdt.h>
#include <arch/percpu.h>
#include <devices/pit.h>
#include <display/framebuffer.h>
#include <drivers/sound/sb16.h>
#include <loader/process.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <msg/msg.h>
#include <sched/sched.h>
#include <stdint.h>
#include <utilities/log.h>
#include <utilities/panic.h>
#include <utilities/string.h>

static int task_set_cwd(struct task *t, const char *path)
{
    size_t i;

    if (t == NULL || t->files == NULL || path == NULL)
        return -1;

    for (i = 0; i + 1 < sizeof(t->files->cwd) && path[i] != '\0'; i++)
        t->files->cwd[i] = path[i];

    t->files->cwd[i] = '\0';

    /*
     * Reject paths that do not fit rather than silently truncating them.
     */
    if (path[i] != '\0')
        return -1;

    return 0;
}

static void task_set_cwd_truncated(struct task *t, const char *path)
{
    size_t i;

    if (t == NULL || t->files == NULL || path == NULL)
        return;

    for (i = 0; i + 1 < sizeof(t->files->cwd) && path[i] != '\0'; i++)
        t->files->cwd[i] = path[i];

    t->files->cwd[i] = '\0';
}

static int task_set_cwd_root(struct task *t)
{
    return task_set_cwd(t, "/");
}

void task_inherit_cwd(struct task *child, struct task *parent)
{
    size_t i;

    if (child == NULL || child->files == NULL)
        return;

    if (parent == NULL ||
        parent->files == NULL ||
        parent->files->cwd[0] == '\0') {
        child->files->cwd[0] = '/';
        child->files->cwd[1] = '\0';
        return;
    }

    for (i = 0;
         i + 1 < sizeof(child->files->cwd) &&
         parent->files->cwd[i] != '\0';
         i++) {
        child->files->cwd[i] = parent->files->cwd[i];
    }

    child->files->cwd[i] = '\0';
}

extern void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp,
                           uint64_t new_cr3, void *old_fxstate,
                           void *new_fxstate);

/* Capture the CPU's current x87/SSE state into `buf`. Used at task creation
 * so a fresh task's first context_switch fxrstors from a valid snapshot
 * instead of zeroed memory (which fxrstor would treat as a reserved-bits
 * fault). */
static void fxstate_init(void *buf) {
  __asm__ volatile("fxsave (%0)" ::"r"(buf) : "memory");
}
extern uint64_t *kernel_pml4;

/* Free a process PML4 , defined in loader/process.c. */
extern void free_user_pml4(uint64_t *pml4);

#define MSR_FS_BASE 0xC0000100u

static void sched_wrmsr(uint32_t msr, uint64_t value) {
  uint32_t lo = (uint32_t)value;
  uint32_t hi = (uint32_t)(value >> 32);
  __asm__ volatile("wrmsr" ::"c"(msr), "a"(lo), "d"(hi) : "memory");
}

static uint64_t sched_rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static struct task tasks[MAX_TASKS];
static int next_pid = 1;
static struct task *current = 0;
/* One round-robin queue per priority level; see SCHED_PRIO_* in sched.h. */
static struct task *ready_head[SCHED_PRIO_LEVELS] = {0};
static struct task *ready_tail[SCHED_PRIO_LEVELS] = {0};
#define SCHED_QUANTUM_MS 10U
static uint32_t slice_ticks = 0;
/* Count of tasks in TASK_SLEEPING. Maintained by task_sleep_ticks /
 * sched_wake_sleepers so the PIT IRQ can skip a full task-table walk on
 * every tick when nothing is asleep , which is the common case. */
static int n_sleeping = 0;

/* --- per-task side allocations ----------------------------------------
 *
 * struct task holds pointers to these rather than the structures inline.
 * Inline, one control block carried a 32-entry descriptor table with a
 * 256-byte path per entry, 64 VMA records, an mmap hole list and a 512-byte
 * FPU save area , about 12 KiB, multiplied by MAX_TASKS whether or not the
 * slot was in use, and every kernel thread paid the same as a full process.
 *
 * Split out, the control block is a couple of hundred bytes and each piece
 * is allocated only by the tasks that need it: kernel threads get no address
 * space, and nothing at all is charged for a free slot.
 *
 * All of them are allocated up front for the tasks that want them, not
 * lazily. Every field below used to be reachable unconditionally, so a NULL
 * that only appears under memory pressure would turn a clean allocation
 * failure at spawn into a fault somewhere later. */
static void task_state_free(struct task *t) {
  if (!t)
    return;

  if (t->input) {
    if (t->input->ring)
      kfree(t->input->ring);
    kfree(t->input);
    t->input = 0;
  }
  if (t->ipc) {
    if (t->ipc->ring)
      kfree(t->ipc->ring);
    kfree(t->ipc);
    t->ipc = 0;
  }
  if (t->files) {
    for (int i = 0; i < TASK_MAX_FDS; i++)
      task_fd_clear(&t->files->fd[i]);
    kfree(t->files);
    t->files = 0;
  }
  if (t->vm) {
    kfree(t->vm);
    t->vm = 0;
  }
  if (t->context) {
    kfree(t->context);
    t->context = 0;
  }
}

/* Allocate the pieces a task needs. `with_vm` is 0 for kernel threads, which
 * run on the kernel address space and never own a PML4 or a VMA list.
 * All-or-nothing: a partial allocation is unwound before returning -1. */
static int task_state_alloc(struct task *t, int with_vm) {
  t->context = (struct task_context *)kmalloc(sizeof(*t->context));
  if (!t->context)
    goto fail;
  memset(t->context, 0, sizeof(*t->context));

  t->files = (struct task_files *)kmalloc(sizeof(*t->files));
  if (!t->files)
    goto fail;
  memset(t->files, 0, sizeof(*t->files));

  t->input = (struct task_input *)kmalloc(sizeof(*t->input));
  if (!t->input)
    goto fail;
  memset(t->input, 0, sizeof(*t->input));
  t->input->ring =
      (struct msg *)kmalloc(sizeof(struct msg) * INPUT_RING_SIZE_LOCAL);
  if (!t->input->ring)
    goto fail;
  memset(t->input->ring, 0, sizeof(struct msg) * INPUT_RING_SIZE_LOCAL);

  t->ipc = (struct task_ipc *)kmalloc(sizeof(*t->ipc));
  if (!t->ipc)
    goto fail;
  memset(t->ipc, 0, sizeof(*t->ipc));
  t->ipc->ring =
      (struct ipc_msg *)kmalloc(sizeof(struct ipc_msg) * IPC_RING_SIZE_LOCAL);
  if (!t->ipc->ring)
    goto fail;
  memset(t->ipc->ring, 0, sizeof(struct ipc_msg) * IPC_RING_SIZE_LOCAL);

  if (with_vm) {
    t->vm = (struct task_vm *)kmalloc(sizeof(*t->vm));
    if (!t->vm)
      goto fail;
    memset(t->vm, 0, sizeof(*t->vm));
    t->vm->shmem_next_va = USER_SHMEM_BASE;
    t->vm->mmap_next_va = USER_MMAP_BASE;
  }

  t->input_owner_restore_pid = -1;
  return 0;

fail:
  log_write("sched: task state alloc failed", KERNEL, LOG_ERROR);
  task_state_free(t);
  return -1;
}

/* Kernel threads take everything except the address space: they still read
 * and write files, still receive IPC, and still need somewhere to fxsave. */
static int alloc_rings_for(struct task *t) { return task_state_alloc(t, 0); }

static void free_rings_for(struct task *t) { task_state_free(t); }

void task_fd_clear(struct task_fd *slot) {
  if (!slot)
    return;
  if (slot->dir_path) {
    kfree(slot->dir_path);
    slot->dir_path = 0;
  }
  slot->type = TASK_FD_UNUSED;
  slot->object = 0;
  slot->dir_index = 0;
  slot->flags = 0;
}

int task_fd_set_dir_path(struct task_fd *slot, const char *path) {
  if (!slot)
    return -1;
  if (!slot->dir_path) {
    slot->dir_path = (char *)kmalloc(TASK_CWD_MAX);
    if (!slot->dir_path)
      return -1;
  }
  size_t i = 0;
  if (path) {
    while (i + 1 < TASK_CWD_MAX && path[i]) {
      slot->dir_path[i] = path[i];
      i++;
    }
  }
  slot->dir_path[i] = 0;
  return 0;
}

/* Idle task lives outside the normal ready queue. ready_pop returns it as
 * a floor when the queue is empty so the CPU always has something to hlt
 * on. Never push it back via ready_push. */
static struct task *idle_task = 0;

static void user_task_trampoline(void);
static void idle_thread(void);
static void mark_task_exited(struct task *task, long code);
extern void arch_enter_user(uint64_t entry, uint64_t user_rsp,
                            uint64_t arg) NORETURN;

static uint64_t irq_save(void) {
  uint64_t rflags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags)::"memory");
  return rflags;
}

static void irq_restore(uint64_t rflags) {
  if (rflags & (1ULL << 9))
    __asm__ volatile("sti" ::: "memory");
}

/* Consecutive dispatches served from HIGH before a runnable NORMAL task is
 * guaranteed a turn. This is what keeps the scheduling weighted instead of
 * strict: winman never blocks , it yields at the bottom of its loop and is
 * immediately runnable again , so strict priority would hand it the CPU
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
   * back for , reclaim them and retry once. This is the path that makes
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
  __asm__ volatile("sti" ::: "memory");
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
static uint64_t build_initial_frame(void *kstack_base,
                                    void (*trampoline)(void)) {
  /* context_switch enters a fresh task with ret, not call. After that ret,
   * the trampoline still has to look like a normal SysV C callee: rsp % 16
   * must be 8 on function entry. */
  uint64_t *sp = (uint64_t *)(kstack_aligned_top(kstack_base) - 8);
  *--sp = (uint64_t)trampoline; /* ret addr */
  *--sp = 0;                    /* rbx */
  *--sp = 0;                    /* rbp */
  *--sp = 0;                    /* r12 */
  *--sp = 0;                    /* r13 */
  *--sp = 0;                    /* r14 */
  *--sp = 0;                    /* r15 */
  return (uint64_t)sp;
}

void sched_init(void) {
  memset(tasks, 0, sizeof(tasks));
  struct cpu_local *cpu = percpu_this();
  /* Bootstrap: caller of sched_init becomes init task. No fake frame
   * needed , its rsp gets captured at the first context_switch. */
  struct task *t = alloc_slot();
  t->pid = next_pid++;
  t->state = TASK_RUNNING;
  t->cr3 = virt_to_phys(kernel_pml4);
  t->kstack = 0;
  t->kthread_entry = 0;
  t->vm = 0;
  /* syscall_init_this_cpu staged the BSP's bootstrap entry stack before the
   * scheduler came online. Keep it in the init task so every task has a
   * complete entry-stack contract even though init itself is kernel-only. */
  t->syscall_kstack_top = cpu->kernel_rsp_top;
  if (alloc_rings_for(t) != 0)
    panic("sched: init task state alloc failed");
  task_set_cwd_root(t);
  fxstate_init(t->context->fxstate);
  task_set_name(t, "init");
  current = t;
  cpu->current = t;
  log_write("sched: init task pid=1 ready", KERNEL, LOG_INFO);

  /* Create idle through the normal spawn path, then detach it from the ready
   * queue. Its hlt loop runs only as ready_pop's fallback when nobody else
   * can run. */
  struct task *idle = task_spawn(idle_thread);
  if (idle) {
    task_set_name(idle, "idle");
    ready_remove(idle);
    idle_task = idle;
    cpu->idle_task = idle;
  }
  log_write("sched: idle task spawned", KERNEL, LOG_INFO);
}

struct task *task_current(void) { return current; }

int task_set_fs_base(uint64_t base) {
  struct task *t = task_current();
  if (!t || !t->context)
    return -1;
  t->context->fs_base = base;
  sched_wrmsr(MSR_FS_BASE, base);
  return 0;
}

uint64_t task_get_fs_base(void) {
  struct task *t = task_current();
  return (t && t->context) ? t->context->fs_base : 0;
}

void task_set_name(struct task *t, const char *name) {
  if (!t)
    return;
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
  if (!out || max <= 0)
    return 0;
  int n = 0;
  for (int i = 0; i < MAX_TASKS && n < max; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0)
      continue;
    out[n].pid = t->pid;
    out[n].parent_pid = t->parent_pid;
    out[n].state = (int)t->state;
    out[n].ticks_run = t->ticks_run;
    for (size_t k = 0; k < sizeof(out[n].name); k++) {
      out[n].name[k] = t->name[k];
    }
    n++;
  }
  return n;
}

struct task *task_find(int pid) {
  if (pid <= 0)
    return 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tasks[i].pid == pid)
      return &tasks[i];
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
  t->state = TASK_READY;
  t->prio = SCHED_PRIO_NORMAL; /* callers raise it explicitly if needed */
  t->pid = next_pid++;
  t->parent_pid = 0;
  t->waiting_for_pid = 0;
  t->exit_code = 0;
  t->kstack = stack_base;
  t->kthread_entry = entry;
  t->vm = 0; /* kernel thread: runs on the kernel address space */
  t->next = 0;

  /* State before cwd: the working directory lives in t->files now, so
   * setting it first would write into a table that does not exist yet. */
  if (alloc_rings_for(t) != 0) {
    kfree(stack_base);
    memset(t, 0, sizeof(*t));
    return 0;
  }
  task_set_cwd_root(t);
  fxstate_init(t->context->fxstate);

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
  t->state = TASK_READY;
  t->prio = SCHED_PRIO_NORMAL;
  t->pid = next_pid++;
  t->parent_pid = parent_pid;
  t->waiting_for_pid = 0;
  t->exit_code = 0;
  t->kstack = stack_base;
  t->kthread_entry = 0;
  t->next = 0;

  if (task_state_alloc(t, 1) != 0) {
    kfree(stack_base);
    memset(t, 0, sizeof(*t));
    return 0;
  }

  t->context->user_entry = entry;
  t->context->user_rsp_initial = user_rsp;
  t->context->fs_base = 0;
  t->vm->user_pml4 = user_pml4;
  t->vm->pml4_ref_count = (int *)kmalloc(sizeof(int));
  if (!t->vm->pml4_ref_count) {
    task_state_free(t);
    kfree(stack_base);
    memset(t, 0, sizeof(*t));
    log_write("sched: pml4 refcount alloc failed", KERNEL, LOG_ERROR);
    return 0;
  }
  *t->vm->pml4_ref_count = 1;

  task_inherit_cwd(t, task_current());
  fxstate_init(t->context->fxstate);

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

  /* The address space arrives later, in task_activate_reserved_user, but the
   * vm record is allocated now: a reservation that cannot get one has failed,
   * and finding that out here is cheaper than unwinding a loaded image. */
  if (task_state_alloc(t, 1) != 0) {
    kfree(stack_base);
    memset(t, 0, sizeof(*t));
    log_write("sched: reserved user state alloc failed", KERNEL, LOG_ERROR);
    return 0;
  }
  task_inherit_cwd(t, task_current());
  fxstate_init(t->context->fxstate);
  return t;
}

int task_activate_reserved_user(struct task *t, uint64_t *user_pml4,
                                uint64_t entry, uint64_t user_rsp) {
  if (!t || t->state != TASK_LOADING || !user_pml4 || !entry)
    return -1;
  if (!t->vm || !t->context)
    return -1;

  int *refs = (int *)kmalloc(sizeof(int));
  if (!refs)
    return -1;
  *refs = 1;

  t->cr3 = virt_to_phys(user_pml4);
  t->vm->user_pml4 = user_pml4;
  t->vm->pml4_ref_count = refs;
  t->context->user_entry = entry;
  t->context->user_rsp_initial = user_rsp;
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
  if (!parent || !parent->vm || !parent->vm->user_pml4 ||
      !parent->vm->pml4_ref_count || !parent->context)
    return 0;

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

  t->cr3 = parent->cr3;

  /* Its kernel stack is its own, though: two threads sharing one would
   * corrupt each other the first time both entered a syscall. */
  t->syscall_kstack_top = stack_top;

  t->state = TASK_READY;
  t->prio = SCHED_PRIO_NORMAL;
  t->pid = next_pid++;
  t->parent_pid = parent->pid;
  t->waiting_for_pid = 0;
  t->exit_code = 0;
  t->kstack = stack_base;
  t->kthread_entry = 0;
  t->next = 0;

  if (task_state_alloc(t, 1) != 0) {
    kfree(stack_base);
    memset(t, 0, sizeof(*t));
    return 0;
  }

  /* A thread shares its parent's address space rather than owning one, so
   * take a reference , whoever exits last frees the PML4.
   *
   * It gets its own vm *record* rather than sharing the parent's, which is
   * what the inline layout did too: the arena cursors and VMA list were
   * per-task there, so sharing them here would be a behaviour change, not a
   * refactor. Only the page tables and their refcount are shared. */
  t->vm->user_pml4 = parent->vm->user_pml4;
  t->vm->pml4_ref_count = parent->vm->pml4_ref_count;
  __atomic_add_fetch(t->vm->pml4_ref_count, 1, __ATOMIC_ACQ_REL);

  t->context->user_entry = entry;
  t->context->user_rsp_initial = user_stack;
  t->context->fs_base = parent->context->fs_base;

  task_inherit_cwd(t, parent);
  fxstate_init(t->context->fxstate);

  ready_push(t);
  return t;
}

/* Stage CPU-local privilege-entry state immediately before the stack swap.
 * A syscall frame owns its saved user RSP once entry is complete, so only the
 * next kernel stack and TSS RSP0 need to follow the scheduled task. */
static void stage_for(struct task *next) {
  struct cpu_local *cpu = percpu_this();
  cpu->kernel_rsp_top = next->syscall_kstack_top;
  cpu->current = next;
  sched_wrmsr(MSR_FS_BASE, next->context ? next->context->fs_base : 0);
  /* Address this CPU's TSS explicitly. The legacy tss_set_rsp0() writes CPU 0
   * unconditionally, which is only harmless while userspace never leaves the
   * BSP. */
  tss_set_rsp0_for(cpu->cpu_id, next->syscall_kstack_top);
}

/* FS is task state rather than entry scratch, so preserve it across switches.
 */
static void capture_from(struct task *prev) {
  if (prev->context)
    prev->context->fs_base = sched_rdmsr(MSR_FS_BASE);
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

  context_switch(&prev->saved_rsp, next->saved_rsp, next->cr3, prev->context->fxstate,
                 next->context->fxstate);
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
    // Fall back to the idle task (BSP) instead of hanging
    current->state = TASK_READY;
    ready_push(current);
    next = ready_pop();
    if (!next) {
      log_write("sched: no runnable task, falling back to idle", KERNEL,
                LOG_ERROR);
      __asm__ volatile("sti; hlt"); // Re-enable interrupts and halt
    }
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

  context_switch(&prev->saved_rsp, next->saved_rsp, next->cr3, prev->context->fxstate,
                 next->context->fxstate);
  irq_restore(rflags);
}

void task_wakeup(struct task *t) {
  if (!t || t->state != TASK_BLOCKED)
    return;
  t->state = TASK_READY;
  t->waiting_for_pid = 0;
  ready_push(t);
}

int task_wake_futex(uint64_t phys) {
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0)
      continue;
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
  if (ticks == 0)
    return;

  uint64_t rflags = irq_save();
  slice_ticks = 0;
  current->wake_tick = pit_ticks() + ticks;
  current->state = TASK_SLEEPING;
  n_sleeping++;

  struct task *next = ready_pop();
  if (!next) {
    /* Nothing else runnable , fall back to halting until the next IRQ.
     * The PIT IRQ that does eventually fire will run sched_wake_sleepers
     * and put us back on the ready queue; we re-poll on the next loop. */
    current->state = TASK_RUNNING;
    n_sleeping--;
    irq_restore(rflags);
    while (pit_ticks() < current->wake_tick)
      __asm__ volatile("hlt");
    return;
  }

  struct task *prev = current;
  capture_from(prev);

  next->state = TASK_RUNNING;
  current = next;
  stage_for(next);

  context_switch(&prev->saved_rsp, next->saved_rsp, next->cr3, prev->context->fxstate,
                 next->context->fxstate);
  irq_restore(rflags);
}

/* Called from the PIT IRQ. Short-circuits when n_sleeping == 0 (the common
 * case) so the IRQ handler isn't doing an MAX_TASKS-wide table walk on every
 * tick just to find nothing to do. */
void sched_wake_sleepers(void) {
  if (n_sleeping == 0)
    return;
  extern uint64_t pit_ticks(void);
  uint64_t now = pit_ticks();
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0)
      continue;
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
void sleep_ms_busy(uint32_t ms) { pit_delay_ms(ms); }

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
    __asm__ volatile("sti; hlt");
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
  if (task->vm && task->vm->user_pml4)
    framebuffer_unregister_user(task->vm->user_pml4, task->pid);

  /* A direct framebuffer process can temporarily take ownership from winman.
   * When it exits, hand ownership back to the saved owner if it still exists.
   */
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
   * , process_spawn_async children and task_kill victims both land here ,
   * so the slot is up for grabs. Without this the slot, the kstack and the
   * whole user PML4 leak, which at MAX_TASKS = 16 is a short road. */
  int claimed = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0)
      continue;
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
  if (!target || !target->vm || !target->vm->user_pml4 ||
      target->state == TASK_ZOMBIE || target->state == TASK_DEAD)
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

  struct task *prev = current;

  if (!prev)
    panic("task_exit: current is NULL");

  if (prev == idle_task)
    panic("task_exit: idle task attempted to exit");

  mark_task_exited(prev, code);

  /*
   * mark_task_exited() must make prev non-runnable:
   *
   *     prev->state = TASK_ZOMBIE;
   *
   * and must not leave it on a ready queue.
   */

  struct task *next = ready_pop();

  if (!next)
    next = idle_task;

  if (!next)
    panic("task_exit: no runnable task and no idle task");

  next->state = TASK_RUNNING;
  current = next;

  log_write_hex("exit prev pid =", (uint64_t)prev->pid, KERNEL, LOG_INFO);
  log_write_hex("exit prev state =", (uint64_t)prev->state, KERNEL, LOG_INFO);
  log_write_hex("exit next pid =", (uint64_t)next->pid,

                KERNEL, LOG_INFO);
  log_write_hex("exit next state =", (uint64_t)next->state, KERNEL, LOG_INFO);

  percpu_this()->current = next;
  stage_for(next);

  /*
   * This function must never return to prev.
   * prev's stack and address space are reaped later.
   */
  uint64_t throwaway;

  context_switch(&throwaway, next->saved_rsp, next->cr3, prev->context->fxstate,
                 next->context->fxstate);

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
    if (t->pid == 0)
      continue;
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
  context_switch(&throwaway, next->saved_rsp, next->cr3, prev->context->fxstate,
                 next->context->fxstate);
  __builtin_unreachable();
}

/* fd 0-2 are the standard streams and were never backed by an allocation.
 * Sockets are not freed here either , they are owned by the socket layer ,
 * so only file and directory slots release anything. task_fd_clear then
 * drops the slot's own directory-path allocation. */
static void task_close_fds(struct task *t) {
  if (!t || !t->files)
    return;
  for (int i = 3; i < TASK_MAX_FDS; i++) {
    struct task_fd *slot = &t->files->fd[i];
    if (slot->type == TASK_FD_FILE || slot->type == TASK_FD_DIRECTORY) {
      if (slot->file)
        kfree(slot->file);
    }
    task_fd_clear(slot);
  }
}

static void task_release_address_space(struct task *t) {
  if (!t || !t->vm || !t->vm->user_pml4)
    return;

  if (!t->vm->pml4_ref_count) {
    free_user_pml4(t->vm->user_pml4);
    t->vm->user_pml4 = 0;
    return;
  }

  int *refs = t->vm->pml4_ref_count;

  /* Atomic: sibling threads can reach this concurrently on different CPUs. */
  int new_count = __atomic_sub_fetch(refs, 1, __ATOMIC_ACQ_REL);
  if (new_count < 0) {
    log_write("sched: pml4 refcount underflow", KERNEL, LOG_ERROR);
  }

  if (new_count <= 0) {
    free_user_pml4(t->vm->user_pml4);
    kfree(refs);
  }

  t->vm->user_pml4 = 0;
  t->vm->pml4_ref_count = 0;
}

void task_reap(struct task *t) {
  if (!t || t->state != TASK_ZOMBIE)
    return;

  task_close_fds(t);

  if (t->kstack)
    kfree(t->kstack);

  /* Address space before the side allocations: the PML4 and its refcount
   * live in t->vm, which task_state_free is about to release. */
  task_release_address_space(t);
  task_state_free(t);

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
    if (t->vm && t->vm->shmem_shared_out > 0)
      continue;
    task_reap(t);
    reaped++;
  }
  return reaped;
}

static void user_task_trampoline(void) {
  uint64_t entry = current->context->user_entry;
  uint64_t rsp = current->context->user_rsp_initial;
  uint64_t arg = current->context->user_arg;
  arch_enter_user(entry, rsp, arg);
}
