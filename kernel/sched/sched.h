/* kernel/sched/sched.h , task table + scheduler surface.
 *
 * Holds:
 *   - struct task        , per-process kernel control block
 *   - enum task_state    , scheduler states
 *   - struct task_snap   , userspace-facing snapshot row (must match
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

#include "utilities/types.h"
#include <msg/msg.h>
#include <stddef.h>
#include <stdint.h>

/* Priority levels. NORMAL is 0 so a zeroed task slot defaults to it , no
 * path can accidentally inherit HIGH by forgetting to initialise.
 *
 * HIGH exists for the display critical path only: the window manager and
 * the framebuffer flush thread. Every client's pixels reach the screen
 * through those two, so leaving them to compete round-robin with their own
 * clients means one busy app decides the whole desktop's frame rate.
 * Scheduling is weighted, not strict , see SCHED_HIGH_BURST , so a spinning
 * HIGH task slows the system down instead of wedging it. */
#define SCHED_PRIO_NORMAL 0
#define SCHED_PRIO_HIGH 1
#define SCHED_PRIO_LEVELS 2

/* BSP scheduler with a fixed 16-slot task table and a singly-linked ready
 * queue. APs service the separate SMP-safe kernel work queue; userspace stays
 * on the BSP until scheduler state is made per-CPU. */
#define MAX_TASKS 32
#define KSTACK_BYTES (16 * 1024)
#define TASK_MAX_FDS 32
#define TASK_CWD_MAX 256
#define TASK_MMAP_HOLES 16
#define MAX_USER_VMAS 64

/* Arena bases come from loader/process.h , see the user address-space map. */
#define INPUT_RING_SIZE_LOCAL 64
#define IPC_RING_SIZE_LOCAL 16

struct socket;
struct fat_file;


/* Scheduler and VM types */
enum task_state {
  TASK_RUNNING,
  TASK_BLOCKED,
  TASK_ZOMBIE,
  TASK_READY,
  TASK_DEAD,
  TASK_SLEEPING,
  TASK_LOADING,
};

struct vm_hole {
  uint64_t base;
  uint64_t len; /* zero means unused */
};

struct user_vma {
  uint64_t start;
  uint64_t end;
  uint64_t pte_flags;
  uint32_t used;
};

/*
 * Address-space state.
 *
 * NULL for kernel-only tasks. May be shared by multiple threads if threading
 * is added later.
 */
struct task_vm {
  uint64_t *user_pml4;
  int *pml4_ref_count;

  uint64_t shmem_next_va;
  uint64_t mmap_next_va;

  struct vm_hole mmap_holes[TASK_MMAP_HOLES];
  struct user_vma vmas[MAX_USER_VMAS];

  int shmem_shared_out;
};


/* File descriptor state */
enum task_fd_type {
  TASK_FD_UNUSED = 0,
  TASK_FD_FILE,
  TASK_FD_DIRECTORY,
  TASK_FD_SOCKET,
};

struct task_fd {
  enum task_fd_type type;
  uint8_t flags;
  uint16_t reserved;

  union {
    struct fat_file *file;
    struct socket *socket;
    void *object;
  };

  uint32_t dir_index;
  char *dir_path; /* allocated only for directory FDs */
};

struct task_files {
  struct task_fd fd[TASK_MAX_FDS];
  char cwd[TASK_CWD_MAX];
};


/* IPC and input state */
struct task_ipc {
  struct ipc_msg *ring;
  volatile uint32_t head;
  volatile uint32_t tail;
};

struct task_input {
  struct msg *ring;
  volatile uint32_t head;
  volatile uint32_t tail;
};

/* Architecture-specific execution state */
struct task_context {
  /*
   * Used by the user-task first-run trampoline.
   */
  uint64_t user_entry;
  uint64_t user_rsp_initial;
  uint64_t user_arg;

  /*
   * IA32_FS_BASE for userspace TLS.
   */
  uint64_t fs_base;

  /*
   * x87/SSE state. Must remain 16-byte aligned for fxsave/fxrstor.
   */
  uint8_t fxstate[512] ALIGNED(16);
};

/// Task control block
struct task {
  // Scheduler/context-switch hot state.
  uint64_t saved_rsp;          /* RSP at time of last context switch. */
  uint64_t cr3;                /* Page table base. */
  uint64_t syscall_kstack_top; /* Kernel stack top for syscalls. */

  enum task_state state;
  int prio;
  int pid;
  uint64_t wake_tick;
  uint64_t ticks_run;

  struct task *next;


  // Process/task relationship.
  int parent_pid;
  int waiting_for_pid;
  int input_owner_restore_pid;

  /* Set at exit when no task was blocked waiting on this pid, meaning the
   * exit code will never be claimed and the slot can be freed by whoever
   * gets there first. Zombies without it belong to a waiting parent that
   * still has to read exit_code. */
  int unclaimed;
  long exit_code;


  // Kernel execution.
  void *kstack;
  void (*kthread_entry)(void);


  /* TTY channel this task reads stdin from and writes console output to.
   * Inherited from the parent alongside cwd, so everything a shell launches
   * shares that shell's terminal. 0 is the kernel-rendered one. */
  int tty;
  char name[16];

  /*
   * Optional or separately allocated state.
   *
   * vm is normally non-NULL for user tasks.
   * files may be NULL for kernel threads without filesystem access.
   * ipc and input may be allocated lazily.
   */
  struct task_vm *vm;
  struct task_files *files;
  struct task_ipc *ipc;
  struct task_input *input;
  struct task_context *context;


  // Futex state.
  uint64_t futex_addr;
};


/* Descriptor-table accessors                                                 */
/*
 * One slot holds a file, a directory or a socket, never more than one: the
 * payload is a union and `type` says which arm is live. Reading the wrong
 * arm returns a valid-looking pointer of the wrong type, so descriptor code
 * goes through these rather than touching the union directly.
 *
 * They also fold in the two checks every call site needs anyway , that the
 * task has a descriptor table at all, and that the fd is in range , so a
 * kernel thread with files == NULL falls out as "no such descriptor".
 */
static inline struct task_fd *task_fd_slot(struct task *t, int fd) {
  if (t == NULL || t->files == NULL || fd < 0 || fd >= TASK_MAX_FDS)
    return NULL;
  return &t->files->fd[fd];
}

/* The fat_file behind a file or directory fd, NULL for anything else. */
static inline struct fat_file *task_fd_file(struct task *t, int fd) {
  struct task_fd *s = task_fd_slot(t, fd);
  if (s == NULL)
    return NULL;
  if (s->type != TASK_FD_FILE && s->type != TASK_FD_DIRECTORY)
    return NULL;
  return s->file;
}

static inline struct socket *task_fd_socket(struct task *t, int fd) {
  struct task_fd *s = task_fd_slot(t, fd);
  if (s == NULL || s->type != TASK_FD_SOCKET)
    return NULL;
  return s->socket;
}

static inline int task_fd_is_dir(struct task *t, int fd) {
  struct task_fd *s = task_fd_slot(t, fd);
  return s != NULL && s->type == TASK_FD_DIRECTORY;
}


/* Address-space accessor                                                     */
/*
 * The page-table root of a task's address space, or NULL if it has none ,
 * a kernel thread, or a reservation whose image has not been activated yet.
 * Folds in the vm NULL check that every caller would otherwise repeat.
 */
static inline uint64_t *task_pml4(struct task *t) {
  if (t == NULL || t->vm == NULL)
    return NULL;
  return t->vm->user_pml4;
}

/* Release a slot's own allocations and mark it unused. Does not close the
 * file or socket , the caller owns that , but it does free the directory
 * path, which is the one thing the slot allocated for itself. */
void task_fd_clear(struct task_fd *slot);

/* Point a directory slot at `path`, allocating its copy. Returns -1 if the
 * copy cannot be allocated, leaving the slot's path unset. */
int task_fd_set_dir_path(struct task_fd *slot, const char *path);


/* Userspace snapshot ABI                                                    */
struct task_snap {
  uint64_t ticks_run;
  int pid;
  int parent_pid;
  int state;
  char name[16];
};

_Static_assert(sizeof(struct task_snap) == 40,
               "task_snap must match userspace proc_info size");

_Static_assert(offsetof(struct task_snap, ticks_run) == 0,
               "task_snap.ticks_run offset is userspace ABI");

_Static_assert(offsetof(struct task_snap, pid) == 8,
               "task_snap.pid offset is userspace ABI");

_Static_assert(offsetof(struct task_snap, name) == 20,
               "task_snap.name offset is userspace ABI");


/* Scheduler API                                                             */
int sched_snapshot(struct task_snap *out, int max);
void task_set_name(struct task *t, const char *name);

void sched_init(void);

void sleep_ms(uint32_t ms);
void sleep_ms_busy(uint32_t ms);

struct task *task_spawn(void (*entry)(void));

struct task *task_spawn_user(uint64_t *user_pml4, uint64_t entry,
                             uint64_t user_rsp, int parent_pid);

struct task *task_reserve_user(int parent_pid);

int task_activate_reserved_user(struct task *t, uint64_t *user_pml4,
                                uint64_t entry, uint64_t user_rsp);

void task_fail_reserved_user(struct task *t, long code);

struct task *task_spawn_thread(uint64_t entry, uint64_t user_stack);

void task_yield(void);
void sched_preempt_tick(void);

void task_block(int waiting_for_pid);
void task_wakeup(struct task *t);
int task_wake_futex(uint64_t phys);

void task_exit(long code) NORETURN;
void task_exit_thread(void) NORETURN;

int task_kill(int pid, long code);

struct task *task_current(void);
struct task *task_find(int pid);

int task_set_fs_base(uint64_t base);
uint64_t task_get_fs_base(void);

int sched_set_priority(struct task *t, int prio);

void task_inherit_cwd(struct task *child, struct task *parent);

/* Copy the parent's TTY channel to the child. Called from the same places
 * as task_inherit_cwd, and for the same reason: a spawned program belongs
 * to the terminal that launched it. Falls back to channel 0 when there is
 * no parent, which is what a kthread wants. */
void task_inherit_tty(struct task *child, struct task *parent);

void task_sleep_ticks(uint64_t ticks);
void sched_wake_sleepers(void);

void task_reap(struct task *t);
int task_reap_unclaimed(void);

/* Entry point for the zombie reaper kthread. Spawn once at boot. */
void task_reaper_thread_entry(void);

#endif
