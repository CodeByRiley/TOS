/* src/impl/kernel/sched/sched.c — task table + scheduler.
 *
 * Single ready queue with PIT-driven preemption. Tasks have explicit
 * states (RUNNING/READY/BLOCKED/SLEEPING/ZOMBIE/DEAD); sleeping tasks
 * sit off the ready queue and get re-queued by sched_wake_sleepers when
 * their wake_tick hits.
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
 * Context switch: assembly in src/impl/x86_64/sched/switch.asm. Saves
 * callee-saved + rsp, swaps to the new task's saved_rsp.
 */
// #region INCLUDES

#include "sched/sched.h"
#include "memory/hhdm.h"
#include "arch/gdt.h"
#include "memory/heap.h"
#include "msg/msg.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

// #endregion INCLUDES

// #region CONSTANTS

#define SHMEM_USER_BASE 0x0000000080000000ULL
#define MMAP_USER_BASE  0x0000000070000000ULL
#define INPUT_RING_SIZE_LOCAL 64
#define IPC_RING_SIZE_LOCAL   16

/* Cooperative-ish round-robin scheduler with a fixed 16-slot task table,
 * a singly-linked ready queue, and the optimism of a startup pitch deck.
 * No priorities, no time slicing, no SMP. The only thing keeping this
 * fair is that nobody's written a hostile kthread yet. */

#define MAX_TASKS 16
#define KSTACK_BYTES (16 * 1024)

// #endregion CONSTANTS

// #region EXTERNS + FXSTATE

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

// #endregion EXTERNS + FXSTATE

// #region TASK TABLE

static struct task tasks[MAX_TASKS];
static int next_pid = 1;
static struct task *current = 0;
static struct task *ready_head = 0;
static struct task *ready_tail = 0;
/* Count of tasks in TASK_SLEEPING. Maintained by task_sleep_ticks /
 * sched_wake_sleepers so the PIT IRQ can skip a full task-table walk on
 * every tick when nothing is asleep — which is the common case. */
static int n_sleeping = 0;

// #endregion TASK TABLE

// #region RING ALLOCATION

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

    t->shmem_next_va = SHMEM_USER_BASE;
    t->mmap_next_va  = MMAP_USER_BASE;
    t->input_owner_restore_pid = -1;
    return 0;
}

static void free_rings_for(struct task *t) {
    if (t->input_ring) { kfree(t->input_ring); t->input_ring = 0; }
    if (t->ipc_ring)   { kfree(t->ipc_ring);   t->ipc_ring   = 0; }
}
// #endregion RING ALLOCATION

// #region READY QUEUE

/* Idle task lives outside the normal ready queue. ready_pop returns it as
 * a floor when the queue is empty so the CPU always has something to hlt
 * on. Never push it back via ready_push. */
static struct task *idle_task = 0;

static void user_task_trampoline(void);
static void idle_thread(void);

static void ready_push(struct task *t) {
  t->next = 0;
  if (!ready_tail)
    ready_head = ready_tail = t;
  else {
    ready_tail->next = t;
    ready_tail = t;
  }
}

static struct task *ready_pop(void) {
  struct task *t = ready_head;
  if (!t)
    return 0;
  ready_head = t->next;
  if (!ready_head)
    ready_tail = 0;
  t->next = 0;
  return t;
}

// #endregion READY QUEUE

// #region SLOT + TRAMPOLINE

static struct task *alloc_slot(void) {
  for (int i = 0; i < MAX_TASKS; i++) {
    if (tasks[i].pid == 0)
      return &tasks[i];
  }
  return 0;
}

/* Trampoline runs on first switch into a fresh kernel thread.
 * Reads entry from current task struct, calls it, exits with rc=0. */
static void kthread_trampoline(void) {
  void (*fn)(void) = current->kthread_entry;
  fn();
  task_exit(0);
}

/* Build a kernel-stack frame matching context_switch's epilogue, which
 * pops r15, r14, r13, r12, rbp, rbx, ret. So we lay out (low → high):
 * [r15][r14][r13][r12][rbp][rbx][ret]. saved_rsp points at r15. */
static uint64_t build_initial_frame(void *kstack_base, void (*trampoline)(void)) {
  uint64_t *sp = (uint64_t *)((uint8_t *)kstack_base + KSTACK_BYTES);
  *--sp = (uint64_t)trampoline;     /* ret addr */
  *--sp = 0;                         /* rbx */
  *--sp = 0;                         /* rbp */
  *--sp = 0;                         /* r12 */
  *--sp = 0;                         /* r13 */
  *--sp = 0;                         /* r14 */
  *--sp = 0;                         /* r15 */
  return (uint64_t)sp;
}

// #endregion SLOT + TRAMPOLINE

// #region SCHED INIT

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
  /* The init task currently uses the boot SYSCALL kstack and the global
   * user_rsp_save scratch. After the very first context_switch out, both
   * will be re-staged for whatever task we switch to. */
  t->syscall_kstack_top = kernel_rsp_top;
  t->user_rsp_saved = 0;
  alloc_rings_for(t);
  fxstate_init(t->fxstate);
  task_set_name(t, "init");
  current = t;
  log_write("sched: init task pid=1 ready", KERNEL, LOG_INFO);

  /* Spawn the idle task. It sits in the ready queue and only runs when
   * nobody else can — its hlt loop is what lets the CPU actually sleep
   * between IRQs instead of busy-yielding. */
  struct task *idle = task_spawn(idle_thread);
  if (idle) task_set_name(idle, "idle");
  log_write("sched: idle task spawned", KERNEL, LOG_INFO);
}

// #endregion SCHED INIT

// #region TASK LOOKUP + SNAPSHOT

struct task *task_current(void) { return current; }

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

// #endregion TASK LOOKUP + SNAPSHOT

// #region TASK SPAWN

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

  t->saved_rsp = build_initial_frame(stack_base, kthread_trampoline);
  t->cr3 = virt_to_phys(kernel_pml4);
  t->syscall_kstack_top = (uint64_t)stack_base + KSTACK_BYTES;
  t->user_rsp_saved = 0;
  t->user_entry = 0;
  t->user_rsp_initial = 0;
  t->state = TASK_READY;
  t->pid = next_pid++;
  t->parent_pid = 0;
  t->waiting_for_pid = 0;
  t->exit_code = 0;
  t->kstack = stack_base;
  t->kthread_entry = entry;
  t->user_pml4 = 0;
  t->next = 0;
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

  t->saved_rsp = build_initial_frame(stack_base, user_task_trampoline);
  t->cr3 = virt_to_phys(user_pml4);
  t->syscall_kstack_top = (uint64_t)stack_base + KSTACK_BYTES;
  t->user_rsp_saved = user_rsp;
  t->user_entry = entry;
  t->user_rsp_initial = user_rsp;
  t->state = TASK_READY;
  t->pid = next_pid++;
  t->parent_pid = parent_pid;
  t->waiting_for_pid = 0;
  t->exit_code = 0;
  t->kstack = stack_base;
  t->kthread_entry = 0;
  t->user_pml4 = user_pml4;
  t->next = 0;
  alloc_rings_for(t);
  fxstate_init(t->fxstate);

  ready_push(t);
  return t;
}

// #endregion TASK SPAWN

// #region CONTEXT STAGING

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
  tss_set_rsp0(next->syscall_kstack_top);
}

/* Symmetric capture: park the global user-rsp scratch into `prev` so the
 * value survives across other tasks running. */
static void capture_from(struct task *prev) {
  prev->user_rsp_saved = user_rsp_save;
}

// #endregion CONTEXT STAGING

// #region YIELD + BLOCK + WAKEUP

void task_yield(void) {
  struct task *next = ready_pop();
  if (!next)
    return; /* nothing else runnable */

  struct task *prev = current;
  prev->state = TASK_READY;
  ready_push(prev);

  capture_from(prev);

  next->state = TASK_RUNNING;
  current = next;
  stage_for(next);

  context_switch(&prev->saved_rsp, next->saved_rsp, next->cr3,
                 prev->fxstate, next->fxstate);
}

void task_block(int waiting_for_pid) {
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
}

void task_wakeup(struct task *t) {
  if (!t || t->state != TASK_BLOCKED) return;
  t->state = TASK_READY;
  t->waiting_for_pid = 0;
  ready_push(t);
}

// #endregion YIELD + BLOCK + WAKEUP

// #region SLEEP + WAKE SLEEPERS

void task_sleep_ticks(uint64_t ticks) {
  extern uint64_t pit_ticks(void);
  if (ticks == 0) return;
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

// #endregion SLEEP + WAKE SLEEPERS

// #region IDLE THREAD

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
    __asm__ volatile ("sti; hlt");
    task_yield();
  }
}

// #endregion IDLE THREAD

// #region TASK EXIT + REAP

void task_exit(long code) {
  /* A direct framebuffer process can temporarily take ownership from winman.
   * When it exits, hand ownership back to the saved owner if it still exists. */
  if (msg_input_owner() == current->pid) {
    int restore_pid = current->input_owner_restore_pid;
    struct task *restore = task_find(restore_pid);
    if (restore_pid > 0 && restore && restore->state != TASK_ZOMBIE) {
      msg_input_owner_force(restore_pid);
    } else {
      msg_input_owner_clear(current->pid);
    }
  }

  int wm_pid = msg_input_owner();
  if (wm_pid > 0 && wm_pid != current->pid) {
    struct ipc_msg note;
    memset(&note, 0, sizeof(note));
    note.type = IPC_PEER_EXITED;
    note.a = current->pid;
    ipc_send(wm_pid, &note, 0);
  }

  current->state = TASK_ZOMBIE;
  current->exit_code = code;

  /* Wake any task that was waiting on us via task_block(self.pid). O(MAX_TASKS)
   * because we don't have a wait-list per pid; with 16 slots this is cheaper
   * than maintaining a real data structure to be cheaper. */
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task *t = &tasks[i];
    if (t->pid == 0) continue;
    if (t->state == TASK_BLOCKED && t->waiting_for_pid == current->pid) {
      task_wakeup(t);
    }
  }

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

void task_reap(struct task *t) {
  if (!t || t->state != TASK_ZOMBIE) return;

  if (t->kstack)    kfree(t->kstack);
  if (t->user_pml4) free_user_pml4(t->user_pml4);
  free_rings_for(t);

  /* pid=0 marks the slot free for alloc_slot. */
  memset(t, 0, sizeof(*t));
}

// #endregion TASK EXIT + REAP

// #region USER TRAMPOLINE

/* User-task first-run trampoline. Runs in ring 0 on the new task's kstack
 * after its first context_switch. Builds an iretq frame using the entry
 * RIP/RSP stashed in the task struct and jumps to ring 3.
 *
 * Every constant in here (0x1B, 0x23, 0x202) is a GDT selector or an RFLAGS
 * value with IF=1. If they look like magic numbers it's because they are
 * magic numbers, and the x86 manual is the spell book. */
static void user_task_trampoline(void) {
  uint64_t entry = current->user_entry;
  uint64_t rsp   = current->user_rsp_initial;

  __asm__ volatile (
      "cli                  \n"
      "mov $0x1B, %%ax      \n"      /* user data | RPL=3 */
      "mov %%ax, %%ds       \n"
      "mov %%ax, %%es       \n"
      "mov %%ax, %%fs       \n"
      "mov %%ax, %%gs       \n"
      "pushq $0x1B          \n"      /* SS */
      "pushq %0             \n"      /* user RSP */
      "pushq $0x202         \n"      /* RFLAGS, IF=1 */
      "pushq $0x23          \n"      /* user CS | RPL=3 */
      "pushq %1             \n"      /* user RIP */
      "iretq                \n"
      :: "r"(rsp), "r"(entry)
      : "rax", "memory"
  );
  __builtin_unreachable();
}

// #endregion USER TRAMPOLINE
