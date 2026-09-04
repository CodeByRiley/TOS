/* kernel/arch/syscall.c , SYSCALL entry + dispatcher.
 *
 * syscall_init programs LSTAR / STAR / SFMASK and enables SCE so the
 * SYSCALL instruction lands at the asm entry trampoline in
 * kernel/arch/x86_64/cpu/syscall.asm. The trampoline saves all GPRs into a
 * syscall_frame, swaps to the per-CPU kernel stack via gs:CPU_LOCAL_*,
 * and calls syscall_dispatch with a pointer to the frame.
 *
 * Dispatch is one giant switch over the SYS_* number in rax. Each arm
 * reads typed args from the frame's caller-saved register slots,
 * validates pointers against the caller's PML4 if needed, performs the
 * action, and writes the result into the frame's rax slot. The tail of the
 * frame is a complete, validated ring-3 iretq image.
 *
 * Pointer validation is best-effort: anything taken from userspace is
 * range-checked against [0, USER_LOW_LIMIT) and walked via
 * vmm_translate_in to confirm it's actually mapped USER+writable. The
 * trade-off is "trusted enough to not crash the kernel"; a real OS would
 * do per-page copy-in/copy-out into kernel buffers.
 */
#include <arch/gdt.h>
#include <arch/percpu.h>
#include <arch/syscall.h>
#include <devices/io.h>
#include <devices/pit.h>
#include <devices/rtc.h>
#include <devices/serial.h>
#include <display/framebuffer.h>
#include <display/tty.h>
#include <drivers/sound/sb16.h>
#include <fs/vfs/vfs.h>
#include <input/keyboard.h>
#include <input/mouse.h>
#include <loader/process.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/uvm.h>
#include <memory/vmm.h>
#include <msg/msg.h>
#include <net/icmp.h>
#include <net/ksocket.h>
#include <net/netmon.h>
#include <sched/sched.h>
#include <stddef.h>
#include <stdint.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* User VA boundaries (USER_FB_BASE, USER_MMAP_BASE, USER_MMAP_LIMIT,
 * USER_VA_MIN/MAX, stack) all come from loader/process.h. */
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define RFLAGS_CF (1ULL << 0)
#define RFLAGS_FIXED (1ULL << 1)
#define RFLAGS_PF (1ULL << 2)
#define RFLAGS_AF (1ULL << 4)
#define RFLAGS_ZF (1ULL << 6)
#define RFLAGS_SF (1ULL << 7)
#define RFLAGS_TF (1ULL << 8)
#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_OF (1ULL << 11)
#define RFLAGS_NT (1ULL << 14)
#define RFLAGS_AC (1ULL << 18)
#define RFLAGS_ID (1ULL << 21)

/* Flags that must never remain active while the SYSCALL entry stub executes.
 * R11 still retains the userspace image, which the return validator sanitizes
 * independently before iretq. */
#define SYSCALL_ENTRY_RFLAGS_MASK                                              \
  (RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_NT | RFLAGS_AC)

_Static_assert((SYSCALL_ENTRY_RFLAGS_MASK & RFLAGS_DF) != 0,
               "SYSCALL assembly relies on FMASK to clear DF before C entry");

/* Arithmetic/debug state belongs to userspace. Privileged and virtual-8086
 * controls are deliberately absent, reserved bits are cleared, and IF plus
 * the architecturally fixed bit are restored below. */
#define USER_RETURN_RFLAGS_ALLOWED                                             \
  (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_TF |     \
   RFLAGS_IF | RFLAGS_DF | RFLAGS_OF | RFLAGS_ID)

#define GDT_RPL_USER 3
#define SYSRET_STAR_BASE ((GDT_USER_CODE | GDT_RPL_USER) - 16)

_Static_assert(SYSRET_STAR_BASE + 8 == (GDT_USER_DATA | GDT_RPL_USER),
               "SYSRET STAR base must produce the ring-3 data selector");
_Static_assert(SYSRET_STAR_BASE + 16 == (GDT_USER_CODE | GDT_RPL_USER),
               "SYSRET STAR base must produce the ring-3 code selector");

#define MAX_SHMEM_PAGES 4096 /* per-call cap; 16 MiB */
#define AUDIO_WRITE_MAX (1024 * 1024)

#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_DIRECTORY 0200000

#define AT_FDCWD (-100)
#define AT_EMPTY_PATH 0x1000

#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

#define DT_DIR 4
#define DT_REG 8

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

#define TIOCGWINSZ 0x5413
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMD_MASK 0x7f

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLNVAL 0x0020

struct fb_info {
  u64 width;
  u64 height;
  u64 pitch;
  u64 bpp;
};

struct linux_iovec {
  void *base;
  u64 len;
};

struct linux_dirent64 {
  u64 d_ino;
  int64_t d_off;
  u16 d_reclen;
  u8 d_type;
  char d_name[];
};

struct linux_kstat {
  u64 st_dev;
  u64 st_ino;
  u64 st_nlink;
  u32 st_mode;
  u32 st_uid;
  u32 st_gid;
  u32 __pad0;
  u64 st_rdev;
  int64_t st_size;
  int64_t st_blksize;
  int64_t st_blocks;
  int64_t st_atime_sec;
  int64_t st_atime_nsec;
  int64_t st_mtime_sec;
  int64_t st_mtime_nsec;
  int64_t st_ctime_sec;
  int64_t st_ctime_nsec;
  int64_t __unused[3];
};

struct linux_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct linux_timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

struct linux_winsize {
  u16 ws_row;
  u16 ws_col;
  u16 ws_xpixel;
  u16 ws_ypixel;
};

struct linux_pollfd {
  int32_t fd;
  int16_t events;
  int16_t revents;
};

/* Linux's x86_64 new_utsname layout. musl conditionally renames the final
 * field, but both public forms use these same six 65-byte arrays. */
struct linux_utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

_Static_assert(sizeof(struct linux_utsname) == 390,
               "Linux x86_64 utsname ABI size");

#define LINUX_SIGSET_BYTES 8
#define LINUX_SIGKILL 9
#define LINUX_SIGSTOP 19

extern void syscall_entry(void);

static int resolve_path(const char *path, char *out, usize max);
static int user_buffer_ok(const void *pointer, u64 bytes, int writable);
static long sys_futex_wait(u32 *addr, u32 expected);
static long sys_futex_wake(u32 *addr);

static void uts_field(char out[65], const char *value) {
  usize i = 0;
  while (i < 64 && value[i]) {
    out[i] = value[i];
    i++;
  }
  out[i] = 0;
}

static long sys_uname(struct linux_utsname *out) {
  if (!out || !user_buffer_ok(out, sizeof(*out), 1))
    return -1;

  memset(out, 0, sizeof(*out));
  uts_field(out->sysname, "TOS");
  uts_field(out->nodename, "tos");
  uts_field(out->release, "development");
  uts_field(out->version, "TOS x86_64");
  uts_field(out->machine, "x86_64");
  uts_field(out->domainname, "localdomain");
  return 0;
}

static long sys_rt_sigaction(int signal,
                             const struct task_signal_action *action,
                             struct task_signal_action *old_action,
                             usize sigset_bytes) {
  struct task *task = task_current();
  if (!task || !task->context || signal < 1 || signal > TASK_SIGNAL_COUNT ||
      sigset_bytes != LINUX_SIGSET_BYTES)
    return -1;
  if (action && (signal == LINUX_SIGKILL || signal == LINUX_SIGSTOP))
    return -1;
  if (action && !user_buffer_ok(action, sizeof(*action), 0))
    return -1;
  if (old_action && !user_buffer_ok(old_action, sizeof(*old_action), 1))
    return -1;

  /* Copy through locals so act == oldact consumes the new disposition before
   * returning the previous one, matching the Linux syscall contract. */
  struct task_signal_action next = {0};
  struct task_signal_action previous =
      task->context->signal_actions[signal - 1];
  if (action)
    next = *action;
  if (action)
    task->context->signal_actions[signal - 1] = next;
  if (old_action)
    *old_action = previous;
  return 0;
}

static int fd_alloc_for(struct task *t, struct vfs_file *f) {
  if (!t || !t->files || !f)
    return -1;
  for (int i = 3; i < TASK_MAX_FDS; i++) {
    struct task_fd *slot = &t->files->fd[i];
    if (slot->type != TASK_FD_UNUSED)
      continue;
    task_fd_clear(slot); /* drops any directory path left by the last owner */
    slot->type = TASK_FD_FILE;
    slot->file = f;
    return i;
  }
  return -1;
}

static long sys_fb_info(struct fb_info *out) {
  if (!out)
    return -1;
  out->width = framebuffer_width();
  out->height = framebuffer_height();
  out->pitch = framebuffer_pitch();
  out->bpp = 32;
  return 0;
}

static long sys_fb_map(void) {
  struct task *t = task_current();
  if (task_pml4(t)) {
    int owner = msg_input_owner();
    if (owner != t->pid) {
      if (t->input_owner_restore_pid < 0) {
        t->input_owner_restore_pid = owner;
      }
      msg_input_owner_force(t->pid);
    }
  }

  /* Framebuffer mappings form a prefix at USER_FB_BASE. Find the first page
     that is absent or points at stale backing, then map only that suffix.
     This remains correct across process_exec (the first probe misses) while
     making a resize that merely exposes more rows proportional to the growth.
   */
  u32 pages = framebuffer_num_pages();
  u32 first_missing = 0;
  if (task_pml4(t)) {
    u32 lo = 0, hi = pages;
    while (lo < hi) {
      u32 mid = lo + (hi - lo) / 2;
      u64 va = USER_FB_BASE + (u64)mid * 4096;
      u64 phys = framebuffer_phys_for_page(mid);
      if (phys && vmm_translate_in(t->vm->user_pml4, va) == phys)
        lo = mid + 1;
      else
        hi = mid;
    }
    first_missing = lo;
  }

  for (u32 i = first_missing; i < pages; i++) {
    u64 phys = framebuffer_phys_for_page(i);
    if (!phys)
      return -1;
    /* The framebuffer pool belongs to the display subsystem. Mark this as a
       borrowed mapping so munmap/task teardown removes the PTE without
       returning the still-live scanout frame to the PMM. */
    if (vmm_map(USER_FB_BASE + (u64)i * 4096, phys,
                VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_SHARED) != 0) {
      return -1;
    }
  }
  return USER_FB_BASE;
}

static int fb_source_span(u64 source_pitch, u64 *source_bytes) {
  u64 width = framebuffer_width();
  u64 height = framebuffer_height();
  if (!source_bytes || width == 0 || height == 0 || source_pitch > UINT32_MAX ||
      source_pitch < width * 4ULL) {
    return -1;
  }

  u64 row_bytes = width * 4ULL;
  if (height > 1 && source_pitch > (UINT64_MAX - row_bytes) / (height - 1))
    return -1;
  *source_bytes = source_pitch * (height - 1) + row_bytes;
  return 0;
}

static long sys_fb_register(const void *pixels, u64 source_pitch) {
  struct task *task = task_current();
  u64 source_bytes;
  if (!task_pml4(task) || msg_input_owner() != task->pid || !pixels ||
      fb_source_span(source_pitch, &source_bytes) != 0 ||
      !user_buffer_ok(pixels, source_bytes, 0)) {
    return -1;
  }

  return framebuffer_register_user(task->vm->user_pml4, task->pid,
                                   (u64)(uintptr_t)pixels, (u32)source_pitch,
                                   source_bytes);
}

static long sys_fb_unregister(void) {
  struct task *task = task_current();
  if (!task_pml4(task))
    return -1;
  return framebuffer_unregister_user(task->vm->user_pml4, task->pid);
}

static long sys_fb_present(const void *pixels, u64 source_pitch,
                           const struct fb_rect *user_rects, u64 rect_count) {
  struct task *task = task_current();
  u64 source_bytes;
  if (!task_pml4(task) || msg_input_owner() != task->pid || !pixels ||
      !user_rects || rect_count == 0 || rect_count > FB_PRESENT_MAX_RECTS ||
      fb_source_span(source_pitch, &source_bytes) != 0) {
    return -1;
  }

  int registered = framebuffer_user_buffer_registered(
      task->vm->user_pml4, task->pid, (u64)(uintptr_t)pixels, (u32)source_pitch,
      source_bytes);
  if ((!registered && !user_buffer_ok(pixels, source_bytes, 0)) ||
      !user_buffer_ok(user_rects, rect_count * sizeof(*user_rects), 0)) {
    return -1;
  }

  /* Snapshot metadata before APs start. They never dereference the caller's
   * rectangle array, and the non-preemptible syscall keeps its address space
   * stable until the synchronous copy completes. */
  struct fb_rect rects[FB_PRESENT_MAX_RECTS];
  memcpy(rects, user_rects, rect_count * sizeof(*user_rects));
  return framebuffer_present_user(task->vm->user_pml4, task->pid,
                                  (u64)(uintptr_t)pixels, (u32)source_pitch,
                                  rects, (u32)rect_count);
}

/* Translate PROT_* into PTE bits. Returns 0 for a protection this VMM
 * cannot express, which callers must treat as an error , 0 is never a
 * legal user PTE flag set (VMM_USER is always required). */
static u64 prot_to_pte(int prot) {
  if (prot == PROT_NONE)
    return VMM_PRESENT | VMM_USER | VMM_NX;
  if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return 0;
  if (!(prot & PROT_READ))
    return 0; /* x86 has no write-only or exec-only  */

  u64 flags = VMM_PRESENT | VMM_USER;
  if (prot & PROT_WRITE)
    flags |= VMM_WRITE;
  if (!(prot & PROT_EXEC))
    flags |= VMM_NX;
  return flags;
}

/* Validate a syscall buffer belonging to the running task and materialise
 * any lazy pages it covers, so a copy-in or copy-out cannot take a nested
 * supervisor-mode fault partway through. The address space itself lives in
 * memory/uvm.h; this only supplies the current task's. */
static int user_buffer_ok(const void *pointer, u64 bytes, int writable) {
  struct task *task = task_current();
  return task && uvm_buffer_ok(task->vm, pointer, bytes, writable);
}

/* mmap(addr, len, prot, flags).
 *
 * Anonymous, private, demand-backed: the reservation covers the address
 * range and pages receive zeroed frames on first access. There is no
 * file-backed mapping; a loader reads section bytes in with read() after
 * mapping it.
 *
 * Without MAP_FIXED, `addr` is ignored and the range comes from the arena.
 * With MAP_FIXED, `addr` must be page-aligned and entirely unmapped. */
static long sys_mmap(u64 addr, long len, int prot, int flags) {
  if (len <= 0)
    return -1;

  struct task *t = task_current();
  if (!task_pml4(t))
    return -1;

  u64 pte_flags = prot_to_pte(prot);
  if (!pte_flags)
    return -1;

  u64 bytes = ((u64)len + 4095) & ~4095ULL;
  if (bytes < (u64)len)
    return -1;

  u64 base = uvm_reserve(t->vm, addr, bytes, pte_flags, (flags & MAP_FIXED) != 0);
  return base ? (long)base : -1;
}

/* mprotect(addr, len, prot).
 *
 * A range the framebuffer has registered is refused outright: its pages are
 * borrowed by the display, so re-permissioning them here is not this task's
 * to do. Everything after that is address-space work, and uvm_protect keeps
 * it all-or-nothing. */
static long sys_mprotect(u64 addr, long len, int prot) {
  if (len <= 0 || (addr & 4095))
    return -1;

  struct task *t = task_current();
  if (!task_pml4(t))
    return -1;

  u64 pte_flags = prot_to_pte(prot);
  if (!pte_flags)
    return -1;

  u64 bytes = ((u64)len + 4095) & ~4095ULL;
  if (bytes < (u64)len)
    return -1;
  if (framebuffer_registered_range_overlaps(t->vm->user_pml4, addr, bytes))
    return -1;

  return uvm_protect(t->vm, addr, bytes, pte_flags);
}

/* munmap(addr, len).
 *
 * Unmapping a range with holes in it is not an error , POSIX says the same
 * , so this reports success as long as the range itself is legal. */
static long sys_munmap(u64 addr, long len) {
  if (len <= 0 || (addr & 4095))
    return -1;

  struct task *t = task_current();
  if (!task_pml4(t))
    return -1;

  u64 bytes = ((u64)len + 4095) & ~4095ULL;
  if (bytes < (u64)len)
    return -1;

  return (long)uvm_release(t->vm, addr, bytes);
}

static long sys_kbd_poll(int *pressed, u16 *key) {
  if (!pressed || !key)
    return -1;
  return keyboard_poll(pressed, key);
}

static long sys_get_ticks(void) {
  return (long)(pit_ticks() * 10); // 100Hz → ms
}

// #endregion INPUT + TIME

// #region MSR + INIT

static void wrmsr(u32 msr, u64 value) {
  u32 lo = (u32)value;
  u32 hi = (u32)(value >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static u64 rdmsr(u32 msr) {
  u32 lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((u64)hi << 32) | lo;
}

void syscall_init_this_cpu(u64 kernel_stack_top) {
  struct cpu_local *cpu = percpu_this();
  if (!cpu || cpu->self != cpu || !kernel_stack_top ||
      (kernel_stack_top & 0xFULL)) {
    log_write("syscall: invalid CPU-local entry stack", KERNEL, LOG_ERROR);
    return;
  }

  cpu->kernel_rsp_top = kernel_stack_top;
  cpu->user_rsp_save = 0;

  wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1); // SCE
  /* SYSRET forms SS as STAR[63:48] + 8 and CS as STAR[63:48] + 16.
   * Keep RPL=3 in the base itself: VirtualBox preserves those selector bits,
   * and a later interrupt return rejects an SS selector with RPL=0. */
  wrmsr(MSR_STAR, ((u64)SYSRET_STAR_BASE << 48) | ((u64)GDT_KERNEL_CODE << 32));
  wrmsr(MSR_LSTAR, (u64)syscall_entry);
  wrmsr(MSR_FMASK, SYSCALL_ENTRY_RFLAGS_MASK);

  log_write("syscalls enabled on CPU", KERNEL, LOG_INFO);
}

static int user_return_address_ok(u64 address, int allow_stack_top) {
  if (address < USER_VA_MIN)
    return 0;
  if (address < USER_VA_MAX)
    return 1;
  if (address >= USER_STACK_LOW &&
      address < USER_STACK_TOP + (u64)allow_stack_top)
    return 1;
  return 0;
}

int syscall_prepare_return(struct syscall_frame *f) {
  struct task *task = task_current();
  const u64 user_cs = GDT_USER_CODE | GDT_RPL_USER;
  const u64 user_ss = GDT_USER_DATA | GDT_RPL_USER;

  if (!f || !task_pml4(task))
    return -1;
  if (f->cs != user_cs || f->ss != user_ss)
    return -1;
  if (!user_return_address_ok(f->rip, 0) || !user_return_address_ok(f->rsp, 1))
    return -1;

  u64 rip_entry = vmm_entry_in(task->vm->user_pml4, f->rip);
  if (!(rip_entry & VMM_PRESENT) || !(rip_entry & VMM_USER) ||
      (rip_entry & VMM_NX))
    return -1;

  f->rflags &= USER_RETURN_RFLAGS_ALLOWED;
  f->rflags |= RFLAGS_FIXED | RFLAGS_IF;
  return 0;
}

/* The console a process talks to. Inherited from its parent at spawn, so a
 * shell and everything it launches share one terminal, and two shells never
 * share theirs. */
static int caller_tty(void) {
  struct task *t = task_current();
  return t ? t->tty : TTY_KERNEL;
}

static long sys_write(long fd, const void *buf, long n) {
  if (!buf || n < 0 || !user_buffer_ok(buf, (u64)n, 0))
    return -1;

  if (fd == 1 || fd == 2) {
    tty_write_ch(caller_tty(), (const char *)buf, (usize)n);

    const char *p = (const char *)buf;
    for (long i = 0; i < n; i++)
      serial_write_char(p[i]);

    return n;
  }

  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !task_fd_file(t, fd) ||
      task_fd_is_dir(t, fd))
    return -1;

  return (long)vfs_write(task_fd_file(t, fd), buf, (usize)n);
}

// #endregion FILE I/O HANDLERS

// #region PROCESS HANDLERS

static long sys_exec(const char *path, char *const argv[]) {
  if (!path)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  return process_exec(resolved, argv);
}

/* Fire-and-forget: returns child pid without blocking on its exit code.
 * Used by the shell to launch windowed apps that should keep running while
 * the user types more commands. */
static long sys_spawn(const char *path, char *const argv[]) {
  if (!path)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  return process_spawn_async(resolved, argv);
}

static long sys_kill(long pid, int signal) {
  if (pid < 1)
    return -1;
  return process_kill(pid, signal);
}

static long sys_exit(long code) {
  log_write_hex("user exit code =", code, KERNEL, LOG_INFO);
  task_exit(code);
  return 0; /* unreachable */
}

static long sys_yield(void) {
  task_yield();
  return 0;
}

static long sys_msg_get(struct msg *out) {
  if (!out)
    return -1;
  return msg_get(out) ? 1 : 0;
}

static long sys_msg_peek(struct msg *out) {
  if (!out)
    return -1;
  return msg_peek(out) ? 1 : 0;
}

static long sys_mouse_pos(int32_t *x, int32_t *y, u8 *buttons) {
  if (x)
    *x = mouse_x();
  if (y)
    *y = mouse_y();
  if (buttons)
    *buttons = mouse_buttons();
  return 0;
}

static long sys_con_write(const char *buf, long n) {
  if (!buf || n < 0)
    return -1;
  tty_write_ch(caller_tty(), buf, (usize)n);
  return n;
}

static long sys_con_clear(void) {
  tty_clear_ch(caller_tty());
  return 0;
}

static long sys_con_push(void) { return tty_push_ch(caller_tty()); }

static long sys_con_pop(void) { return tty_pop_ch(caller_tty()); }

static long sys_con_zoom(long delta) {
  return tty_zoom_ch(caller_tty(), (int)delta);
}

static long sys_sleep_ticks(long n) {
  if (n <= 0) {
    task_yield();
    return 0;
  }
  task_sleep_ticks((u64)n);
  return 0;
}

static long sys_get_pid(void) {
  struct task *t = task_current();
  return t ? t->pid : -1;
}

static int audio_caller_pid(void) {
  struct task *task = task_current();
  return task ? task->pid : -1;
}

static long sys_audio_open(long sample_rate, long channels, long format) {
  return sb16_stream_open(audio_caller_pid(), (u32)sample_rate, (u32)channels,
                          (u32)format);
}

static long sys_audio_write(const void *pcm, long bytes) {
  if (bytes < 0 || bytes > AUDIO_WRITE_MAX ||
      !user_buffer_ok(pcm, (u64)bytes, 0))
    return SB16_STREAM_ERR_INVALID;
  return sb16_stream_write(audio_caller_pid(), pcm, (u32)bytes);
}

static long sys_audio_status(struct audio_status_user *out) {
  if (!user_buffer_ok(out, sizeof(*out), 1))
    return SB16_STREAM_ERR_INVALID;

  struct sb16_stream_status status;
  int result = sb16_stream_status(&status);
  if (result != 0)
    return result;

  *out = (struct audio_status_user){
      .available = status.available,
      .playing = status.playing,
      .paused = status.paused,
      .sample_rate = status.sample_rate,
      .channels = status.channels,
      .format = status.format,
      .ring_capacity = status.ring_capacity,
      .ring_queued = status.ring_queued,
      .device_queued = status.device_queued,
      .underruns = status.underruns,
      .volume = status.volume,
      .owner_pid = status.owner_pid,
  };
  return 0;
}

static long sys_audio_drain(void) {
  int pid = audio_caller_pid();
  int result = sb16_stream_begin_drain(pid);
  if (result != 0)
    return result;

  while (sb16_stream_pending(pid) != 0)
    task_sleep_ticks(1);
  return sb16_stream_finish_drain(pid);
}

static long sys_audio_close(void) {
  return sb16_stream_close(audio_caller_pid());
}

static long sys_audio_set_volume(long percent) {
  return sb16_stream_set_volume(audio_caller_pid(), (int)percent);
}

static long sys_audio_pause(void) {
  return sb16_stream_pause(audio_caller_pid());
}

static long sys_audio_resume(void) {
  return sb16_stream_resume(audio_caller_pid());
}

// #endregion CONSOLE + SLEEP + PID

// #region IPC + SHMEM + WINMAN

static long sys_ipc_send(long target_pid, const struct ipc_msg *m) {
  if (!m)
    return -1;
  struct task *t = task_current();
  if (!t)
    return -1;
  return ipc_send((int)target_pid, m, t->pid);
}

static long sys_ipc_recv(struct ipc_msg *out) {
  if (!out)
    return -1;
  return ipc_recv(out) ? 1 : 0;
}

/* Share `npages` worth of the caller's pages starting at `in_va` (page-
 * aligned) into the target process's address space. Kernel picks the
 * target VA via the target task's bump allocator. */
static long sys_shmem_share(long target_pid, u64 in_va, long npages,
                            u64 *out_target_va) {
  if (!out_target_va || target_pid <= 0 || npages <= 0)
    return -1;
  if (npages > MAX_SHMEM_PAGES)
    return -1;
  if (in_va & 0xFFFULL)
    return -1;

  struct task *me = task_current();
  if (!task_pml4(me))
    return -1;

  struct task *target = task_find((int)target_pid);
  if (!task_pml4(target))
    return -1;
  /* Validate the result slot before creating mappings or changing accounting. */
  if (!user_buffer_ok(out_target_va, sizeof(*out_target_va), 1))
    return -1;
  if (target->vm->shmem_next_va == 0)
    target->vm->shmem_next_va = 0x0000000080000000ULL; /* defensive */

  u64 target_va = target->vm->shmem_next_va;
  /* VMM_SHARED marks the PTE so the target's exit cleanup (free_user_pml4)
   * skips pmm_free_frame on these phys frames , the caller still owns them. */
  u64 flags = VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_SHARED;

  for (long i = 0; i < npages; i++) {
    u64 phys = vmm_translate_in(me->vm->user_pml4, in_va + (u64)i * 4096);
    if (!phys)
      return -1;
    if (vmm_map_in(target->vm->user_pml4, target_va + (u64)i * 4096, phys,
                   flags) != 0) {
      return -1;
    }
  }

  target->vm->shmem_next_va = target_va + (u64)npages * 4096;
  /* These frames are now co-mapped by a task that will not free them.
   * Recording that keeps the automatic reaper from releasing our address
   * space underneath the receiver. */
  me->vm->shmem_shared_out += (int)npages;
  *out_target_va = target_va;
  return 0;
}

/* Unmap shared pages from the target process. `va` is an address in the
 * TARGET's address space , the value sys_shmem_share wrote back through
 * out_target_va, not the caller's own mapping of the same frames.
 *
 * The caller still owns the physical frames (they were marked VMM_SHARED
 * at map time), so this does NOT call pmm_free_frame. The caller is
 * responsible for freeing its own mapping via sys_munmap or
 * aligned_page_free.
 *
 * Pages in the range that are not currently shared are skipped rather
 * than treated as an error: a caller tearing down a surface should not
 * have to track exactly which pages survived. */
static long sys_shmem_unshare(long target_pid, u64 va, long npages) {
  if (target_pid <= 0 || npages <= 0 || npages > MAX_SHMEM_PAGES)
    return -1;
  if (va & 0xFFFULL)
    return -1;

  struct task *me = task_current();
  if (!task_pml4(me))
    return -1;

  struct task *target = task_find((int)target_pid);
  if (!task_pml4(target))
    return -1;

  /* Confine the range to the shmem arena, where sys_shmem_share places
   * every mapping it makes.
   *
   * This is not tidiness. process_pml4_create copies PML4[256..511] and
   * the low-1 GiB PDPT entry into every process by physical address, so
   * those page tables are the same memory in all of them. Unmapping an
   * address down there would clear a PTE the kernel and every other
   * process walk, not just this target's view of it. */
  u64 bytes = (u64)npages * 4096;
  if (va < USER_SHMEM_BASE || !uvm_range_ok(va, bytes))
    return -1;

  /* Only pages carrying VMM_SHARED arrived through a share, so only those
   * are ours to take away. Without this check any task could hand another
   * task's pid to unshare and unmap its stack or its text.
   *
   * It still does not prove *this* task was the one that shared them ,
   * that needs per-share ownership the kernel does not record yet , but
   * it keeps the blast radius inside genuinely shared pages. */
  long removed = 0;
  for (long i = 0; i < npages; i++) {
    u64 target_va = va + (u64)i * 4096;
    u64 entry = vmm_entry_in(target->vm->user_pml4, target_va);
    if (!entry || !(entry & VMM_SHARED))
      continue;
    vmm_unmap_in(target->vm->user_pml4, target_va);
    removed++;
  }

  /* Decrement by what actually went away, and never past zero.
   * task_reap_unclaimed reads a positive count as "receivers still map my
   * frames" and declines to reap; a count driven negative would let this
   * task be reaped and free_user_pml4 hand those frames back to the PMM
   * while a receiver was still writing through them. */
  if (removed >= me->vm->shmem_shared_out)
    me->vm->shmem_shared_out = 0;
  else
    me->vm->shmem_shared_out -= (int)removed;

  return 0;
}

static long sys_wm_register(void) {
  struct task *t = task_current();
  if (!t)
    return -1;
  int rc = msg_input_owner_register(t->pid);
  if (rc == 0) {
    tty_set_active(0);
    /* The compositor is on every other process's path to the screen, so it
     * gets the display priority class. Only the first registrant wins the
     * WM role, so this is not a way for any process to promote itself. */
    sched_set_priority(t, SCHED_PRIO_HIGH);
  }
  return rc;
}

static long sys_wm_pid(void) { return msg_input_owner(); }

static long sys_tty_drain(long idx, char *out, long max) {
  if (!out || max <= 0)
    return 0;
  return (long)tty_drain_ch((int)idx, out, (usize)max);
}

/* Claim a spare console channel. Only the process holding the WM role gets
 * one: channels are a fixed, small resource and the window manager is the
 * only thing that can render one, so handing them to arbitrary callers just
 * lets any program exhaust the table. */
static long sys_tty_alloc(void) {
  struct task *t = task_current();
  if (!t || t->pid != msg_input_owner())
    return -1;
  return tty_alloc();
}

static long sys_tty_free(long idx) {
  struct task *t = task_current();
  if (!t || t->pid != msg_input_owner())
    return -1;
  if (idx <= TTY_KERNEL || idx >= TTY_MAX)
    return -1;
  tty_release((int)idx);
  return 0;
}

static long sys_tty_spawn(const char *path, char *const argv[], long idx) {
  struct task *t = task_current();
  if (!t || t->pid != msg_input_owner())
    return -1;
  if (!path)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  return process_spawn_async_tty(resolved, argv, (int)idx);
}

/* Drain the keystrokes winman has injected for whoever owns the console.
 * Non-blocking: returns 0 when nothing is queued.
 *
 * This is the counterpart to SYS_TTY_INJECT, and the reason the console
 * shell must not poll the raw keyboard ring , that ring receives every key
 * regardless of which window has focus, so a shell reading it also collects
 * everything typed into other applications. */
static long sys_tty_read_input(char *out, long max) {
  if (!out || max <= 0)
    return 0;
  return (long)tty_read_input_ch(caller_tty(), out, (usize)max);
}

/* Block-wait `seconds` using PIT ticks (10ms each at 100Hz). */
static void delay_seconds(long seconds) {
  if (seconds <= 0)
    return;
  extern u64 pit_ticks(void);
  u64 target = pit_ticks() + (u64)seconds * 100;
  while (pit_ticks() < target)
    __asm__ volatile("hlt");
}

/* Try several poweroff mechanisms; halt if all fail. */
static void hw_shutdown(void) {
  log_write("powering off...", KERNEL, LOG_INFO);
  if (vfs_sync_all()) {
    log_write("shutdown: filesystem sync failed; refusing poweroff", KERNEL,
              LOG_ERROR);
    return;
  }
  outw(0x604, 0x2000);  /* QEMU >= 2.0 (PIIX ACPI) */
  outw(0xB004, 0x2000); /* Bochs / old QEMU */
  outw(0x4004, 0x3400); /* VirtualBox */
  outw(0x600, 0x34);    /* Cloud Hypervisor */
  __asm__ volatile("cli");
  for (;;)
    __asm__ volatile("hlt");
}

/* Reboot via 8042, then ACPI reset, then triple fault. */
static void hw_reboot(void) {
  log_write("rebooting...", KERNEL, LOG_INFO);
  if (vfs_sync_all()) {
    log_write("reboot: filesystem sync failed; refusing reset", KERNEL,
              LOG_ERROR);
    return;
  }
  /* drain 8042 input buffer */
  while (inb(0x64) & 0x02) {
    (void)inb(0x60);
  }
  outb(0x64, 0xFE);  /* keyboard controller pulse */
  outb(0xCF9, 0x06); /* PIIX/q35 reset */
  /* triple-fault: null IDT + INT3 */
  struct PACKED {
    u16 limit;
    u64 base;
  } idtr = {0, 0};
  __asm__ volatile("lidt %0" : : "m"(idtr));
  __asm__ volatile("int3");
  for (;;)
    __asm__ volatile("hlt");
}

static long sys_shutdown(long time, const char *reason) {
  log_write_hex("shutdown in (s) =", time, KERNEL, LOG_INFO);
  if (reason && *reason) {
    char buf[80];
    const usize prefix = 8;
    memcpy(buf, "reason: ", prefix);
    usize rl = strlen(reason);
    if (rl > sizeof(buf) - prefix - 1)
      rl = sizeof(buf) - prefix - 1;
    memcpy(buf + prefix, reason, rl);
    buf[prefix + rl] = 0;
    log_write(buf, KERNEL, LOG_INFO);
  }
  delay_seconds(time);
  hw_shutdown();
  return -5; /* EIO: sync failed, so poweroff was refused. */
}

static long sys_reboot(long time) {
  log_write_hex("reboot in (s) =", time, KERNEL, LOG_INFO);
  delay_seconds(time);
  hw_reboot();
  return -5; /* EIO: sync failed, so reset was refused. */
}

static int path_is_absolute(const char *path) { return path && path[0] == '/'; }

static int path_append_component(char *out, usize *len, usize max,
                                 const char *start, usize n) {
  if (n == 0 || (n == 1 && start[0] == '.'))
    return 0;

  if (n == 2 && start[0] == '.' && start[1] == '.') {
    if (*len <= 1) {
      out[0] = '/';
      out[1] = 0;
      *len = 1;
      return 0;
    }

    while (*len > 1 && out[*len - 1] != '/')
      (*len)--;
    if (*len > 1)
      (*len)--;
    out[*len] = 0;
    return 0;
  }

  if (*len > 1) {
    if (*len + 1 >= max)
      return -1;
    out[(*len)++] = '/';
  }

  if (*len + n >= max)
    return -1;
  for (usize i = 0; i < n; i++)
    out[(*len)++] = start[i];
  out[*len] = 0;
  return 0;
}

static int resolve_path(const char *path, char *out, usize max) {
  if (!path || !out || max < 2)
    return -1;

  struct task *t = task_current();
  if (!t || !t->files)
    return -1;

  usize len = 0;
  out[0] = '/';
  out[1] = 0;
  len = 1;

  if (!path_is_absolute(path)) {
    len = 0;
    while (len < max - 1 && t->files->cwd[len]) {
      out[len] = t->files->cwd[len];
      len++;
    }
    out[len] = 0;
    if (len == 0) {
      out[0] = '/';
      out[1] = 0;
      len = 1;
    }
  }

  const char *p = path;
  while (*p) {
    while (*p == '/' || *p == '\\')
      p++;

    const char *start = p;
    while (*p && *p != '/' && *p != '\\')
      p++;

    if (path_append_component(out, &len, max, start, (usize)(p - start)) != 0)
      return -1;
  }

  return 0;
}

static long sys_open(const char *path, int flags) {
  struct task *t = task_current();
  if (!t)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  struct vfs_file *f = (struct vfs_file *)kmalloc(sizeof(*f));
  if (!f)
    return -1;
  memset(f, 0, sizeof(*f));

  int rc = vfs_open(resolved, f);
  if (rc == 0 && f->type == VFS_NODE_DIRECTORY) {
    if ((flags & (O_CREAT | O_TRUNC)) != 0) {
      vfs_close(f);
      kfree(f);
      return -1;
    }
    int fd = fd_alloc_for(t, f);
    if (fd < 0) {
      vfs_close(f);
      kfree(f);
      return -1;
    }
    /* Directory fds carry their path so readdir and stat can re-walk it.
     * That path is now a per-slot allocation rather than a fixed 256-byte
     * array in every one of the 32 slots, so it can fail. */
    struct task_fd *slot = task_fd_slot(t, fd);
    if (task_fd_set_dir_path(slot, resolved) != 0) {
      task_fd_clear(slot);
      vfs_close(f);
      kfree(f);
      return -1;
    }
    slot->type = TASK_FD_DIRECTORY;
    slot->file = f;
    slot->dir_index = 0;
    return fd;
  }

  if (rc == 0 && (flags & O_DIRECTORY)) {
    vfs_close(f);
    kfree(f);
    return -1;
  }

  if (rc != 0 && (flags & O_CREAT)) {
    rc = vfs_create(resolved, f);
  } else if (rc == 0 && (flags & O_TRUNC)) {
    rc = vfs_truncate(f);
  }

  if (rc != 0) {
    vfs_close(f);
    kfree(f);
    return -1;
  }

  int fd = fd_alloc_for(t, f);
  if (fd < 0) {
    vfs_close(f);
    kfree(f);
    return -1;
  }

  return fd;
}

static long sys_unlink(const char *path) {
  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return vfs_unlink(resolved);
}

static long sys_rmdir(const char *path) {
  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return vfs_rmdir(resolved);
}

static long sys_read(int fd, void *buf, usize n) {
  struct task *t = task_current();

  /* Handle standard input (fd 0) */
  if (fd == 0) {
    if (!t || !buf || n <= 0 || !user_buffer_ok(buf, n, 1))
      return -1;
    return sys_tty_read_input((char *)buf, (long)n);
  }

  /* Handle normal files (fd >= 3) */
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !task_fd_file(t, fd) ||
      task_fd_is_dir(t, fd))
    return -1;

  struct vfs_file *file = task_fd_file(t, fd);
  usize available =
      file->position < file->size ? (usize)(file->size - file->position) : 0;
  usize transfer = n < available ? n : available;
  if (!user_buffer_ok(buf, transfer, 1))
    return -1;
  return (long)vfs_read(file, buf, n);
}

// static long sys_read(int fd, void *buf, usize n) {
//   struct task *t = task_current();
//   if (!t || fd < 3 || fd >= TASK_MAX_FDS || !task_fd_file(t, fd) ||
//   task_fd_is_dir(t, fd))
//     return -1;

//   struct fat_file *file = task_fd_file(t, fd);
//   usize available = file->pos < file->size ? file->size - file->pos : 0;
//   usize transfer = n < available ? n : available;
//   if (!user_buffer_ok(buf, transfer, 1))
//     return -1;
//   return (long)fat_read(file, buf, n);
// }

static void stat_from_vfs(const struct vfs_stat *fs, struct stat_user *out) {
  out->size = fs->size;
  out->first_cluster = (u32)fs->inode;
  out->type = fs->type == VFS_NODE_DIRECTORY ? STAT_TYPE_DIR : STAT_TYPE_FILE;
  out->attr = fs->attributes;
}

static void linux_stat_from_vfs(const struct vfs_stat *fs,
                                struct linux_kstat *out) {
  memset(out, 0, sizeof(*out));
  out->st_ino = fs->inode ? fs->inode : 1;
  out->st_nlink = 1;
  out->st_mode = fs->mode;
  out->st_size = fs->type == VFS_NODE_DIRECTORY ? 0 : fs->size;
  out->st_blksize = fs->block_size ? fs->block_size : 4096;
  out->st_blocks = fs->blocks;
}

static long sys_stat_raw(const char *path, struct stat_user *out) {
  if (!path || !out)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  struct vfs_stat fs;
  if (vfs_stat(resolved, &fs) != 0)
    return -1;

  stat_from_vfs(&fs, out);
  return 0;
}

static long sys_stat(const char *path, struct linux_kstat *out) {
  if (!path || !out)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  struct vfs_stat fs;
  if (vfs_stat(resolved, &fs) != 0)
    return -1;

  if (!user_buffer_ok(out, sizeof(*out), 1))
    return -1;
  linux_stat_from_vfs(&fs, out);
  return 0;
}

/* fstat works off the open handle rather than re-resolving a path, so it
 * still reports the right size for a file that has been written through
 * this fd , and keeps working if the name is unlinked while open. */
static long sys_fstat_raw(int fd, struct stat_user *out) {
  struct task *t = task_current();
  if (!t || !out || fd < 3 || fd >= TASK_MAX_FDS || !task_fd_file(t, fd))
    return -1;

  if (task_fd_is_dir(t, fd)) {
    struct vfs_stat fs;
    if (vfs_stat(task_fd_slot(t, fd)->dir_path, &fs) != 0)
      return -1;
    stat_from_vfs(&fs, out);
  } else {
    struct vfs_stat fs;
    if (vfs_file_stat(task_fd_file(t, fd), &fs) != 0)
      return -1;
    stat_from_vfs(&fs, out);
  }
  return 0;
}

static long sys_fstat(int fd, struct linux_kstat *out) {
  struct task *t = task_current();
  if (!t || !out || fd < 3 || fd >= TASK_MAX_FDS || !task_fd_file(t, fd))
    return -1;
  if (!user_buffer_ok(out, sizeof(*out), 1))
    return -1;

  struct vfs_stat fs;
  if (task_fd_is_dir(t, fd)) {
    if (vfs_stat(task_fd_slot(t, fd)->dir_path, &fs) != 0)
      return -1;
  } else {
    if (vfs_file_stat(task_fd_file(t, fd), &fs) != 0)
      return -1;
  }
  linux_stat_from_vfs(&fs, out);
  return 0;
}

static long sys_lseek(int fd, long off, int whence) {
  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !task_fd_file(t, fd) ||
      task_fd_is_dir(t, fd))
    return -1;
  struct vfs_file *f = task_fd_file(t, fd);
  u64 base;
  if (whence == 0)
    base = 0;
  else if (whence == 1)
    base = f->position;
  else if (whence == 2)
    base = f->size;
  else
    return -1;
  u64 target;
  if (off < 0) {
    u64 distance = (u64)(-(off + 1)) + 1;
    if (distance > base)
      return -1;
    target = base - distance;
  } else {
    if ((u64)off > UINT64_MAX - base)
      return -1;
    target = base + (u64)off;
  }
  if (vfs_seek(f, target) != 0)
    return -1;
  return (long)target;
}

static long sys_close(int fd) {
  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS)
    return -1;

  struct task_fd *slot = task_fd_slot(t, fd);
  if (!slot)
    return -1;

  /* Sockets share this fd space, so close() has to release them or every
   * socket a program opens outlives it. */
  if (slot->type == TASK_FD_SOCKET) {
    socket_close(slot->socket);
    task_fd_clear(slot);
    return 0;
  }

  if (slot->type != TASK_FD_FILE && slot->type != TASK_FD_DIRECTORY)
    return -1;
  if (slot->file) {
    vfs_close(slot->file);
    kfree(slot->file);
  }
  task_fd_clear(slot);
  return 0;
}

static long sys_readdir(u32 *index, char *buf, usize n) {
  struct task *t = task_current();
  if (!t || !t->files)
    return -1;
  return vfs_read_dir(t->files->cwd, index, buf, n);
}

static long sys_readdir_path(const char *path, u32 *index, char *buf, usize n) {
  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return vfs_read_dir(resolved, index, buf, n);
}

static usize align_up_size(usize value, usize alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static long sys_getdents64(int fd, void *user_buf, usize len) {
  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !task_fd_is_dir(t, fd))
    return -1;
  if (!user_buf || len < sizeof(struct linux_dirent64) + 2 ||
      !user_buffer_ok(user_buf, len, 1))
    return -1;

  char *out = (char *)user_buf;
  usize written = 0;
  for (;;) {
    u32 before = task_fd_slot(t, fd)->dir_index;
    struct vfs_dirent entry;
    long rc = vfs_read_dir_one(task_fd_slot(t, fd)->dir_path,
                               &task_fd_slot(t, fd)->dir_index, &entry);
    if (rc == 0)
      break;
    if (rc < 0)
      return written ? (long)written : -1;

    usize name_len = strlen(entry.name);
    usize reclen = align_up_size(
        offsetof(struct linux_dirent64, d_name) + name_len + 1, 8);
    if (reclen > len - written) {
      task_fd_slot(t, fd)->dir_index = before;
      break;
    }

    struct linux_dirent64 *de = (struct linux_dirent64 *)(out + written);
    memset(de, 0, reclen);
    de->d_ino = entry.inode ? entry.inode : 1;
    de->d_off = (int64_t)task_fd_slot(t, fd)->dir_index;
    de->d_reclen = (u16)reclen;
    de->d_type = entry.type == VFS_NODE_DIRECTORY ? DT_DIR : DT_REG;
    memcpy(de->d_name, entry.name, name_len + 1);
    written += reclen;
  }

  return (long)written;
}

static long sys_brk(u64 addr) {
  (void)addr;
  return -1;
}

static long sys_readv(int fd, const struct linux_iovec *iov, long iovcnt) {
  if (iovcnt < 0 || iovcnt > 1024)
    return -1;
  if (iovcnt == 0)
    return 0;
  if (!user_buffer_ok(iov, (u64)iovcnt * sizeof(*iov), 0))
    return -1;

  long total = 0;
  for (long i = 0; i < iovcnt; i++) {
    if (iov[i].len == 0)
      continue;
    if (iov[i].len > (u64)INT64_MAX)
      return total ? total : -1;
    long rc = sys_read(fd, iov[i].base, (usize)iov[i].len);
    if (rc < 0)
      return total ? total : rc;
    total += rc;
    if (rc == 0 || (u64)rc != iov[i].len)
      break;
  }
  return total;
}

static long sys_writev(int fd, const struct linux_iovec *iov, long iovcnt) {
  if (iovcnt < 0 || iovcnt > 1024)
    return -1;
  if (iovcnt == 0)
    return 0;
  if (!user_buffer_ok(iov, (u64)iovcnt * sizeof(*iov), 0))
    return -1;

  long total = 0;
  for (long i = 0; i < iovcnt; i++) {
    if (iov[i].len == 0)
      continue;
    if (iov[i].len > (u64)INT64_MAX)
      return total ? total : -1;
    long rc = sys_write(fd, iov[i].base, (long)iov[i].len);
    if (rc < 0)
      return total ? total : rc;
    total += rc;
    if ((u64)rc != iov[i].len)
      break;
  }
  return total;
}

static int fd_is_known(struct task *t, int fd) {
  if (!t || fd < 0 || fd >= TASK_MAX_FDS)
    return 0;
  return fd < 3 || task_fd_file(t, fd) != 0;
}

static long sys_fcntl(int fd, int cmd, long arg) {
  struct task *t = task_current();
  if (!fd_is_known(t, fd))
    return -1;

  switch (cmd) {
  case F_GETFD:
  case F_SETFD:
  case F_SETFL:
    return 0;
  case F_GETFL:
    return (fd >= 3 && task_fd_is_dir(t, fd)) ? O_DIRECTORY : 0;
  case F_DUPFD:
    (void)arg;
    return -1;
  default:
    return -1;
  }
}

static long sys_poll(struct linux_pollfd *fds, long nfds, long timeout_ms) {
  if (nfds < 0 || nfds > 64)
    return -1;
  if (nfds == 0) {
    if (timeout_ms > 0) {
      u64 freq = pit_get_freq();
      if (!freq)
        freq = 100;
      u64 ticks = ((u64)timeout_ms * freq + 999) / 1000;
      if (ticks)
        task_sleep_ticks(ticks);
    }
    return 0;
  }
  if (!user_buffer_ok(fds, (u64)nfds * sizeof(*fds), 1))
    return -1;

  struct task *t = task_current();
  long ready = 0;
  for (long i = 0; i < nfds; i++) {
    fds[i].revents = 0;
    if (!fd_is_known(t, fds[i].fd)) {
      fds[i].revents = POLLNVAL;
      ready++;
      continue;
    }
    if ((fds[i].events & POLLOUT) && fds[i].fd != 0)
      fds[i].revents |= POLLOUT;
    if ((fds[i].events & POLLIN) && fds[i].fd >= 3)
      fds[i].revents |= POLLIN;
    if (fds[i].revents)
      ready++;
  }

  if (!ready && timeout_ms > 0) {
    u64 freq = pit_get_freq();
    if (!freq)
      freq = 100;
    u64 ticks = ((u64)timeout_ms * freq + 999) / 1000;
    if (ticks)
      task_sleep_ticks(ticks);
  }
  return ready;
}

static long sys_ioctl(int fd, long request, void *arg) {
  struct task *t = task_current();
  if (!fd_is_known(t, fd))
    return -1;

  if (request == TIOCGWINSZ) {
    if (fd > 2)
      return -1;
    if (!user_buffer_ok(arg, sizeof(struct linux_winsize), 1))
      return -1;
    struct linux_winsize *ws = (struct linux_winsize *)arg;
    ws->ws_row = 25;
    ws->ws_col = 80;
    ws->ws_xpixel = (u16)framebuffer_width();
    ws->ws_ypixel = (u16)framebuffer_height();
    return 0;
  }

  if (request == TCGETS) {
    if (fd > 2)
      return -1;
    if (!user_buffer_ok(arg, 64, 1))
      return -1;
    memset(arg, 0, 64);
    return 0;
  }

  if (request == TCSETS || request == TCSETSW || request == TCSETSF)
    return fd <= 2 ? 0 : -1;

  return -1;
}

/* Uptime. This is what CLOCK_MONOTONIC means and all it should ever mean. */
static void monotonic_timespec(struct linux_timespec *ts) {
  u64 freq = pit_get_freq();
  u64 ticks = pit_ticks();
  if (!freq)
    freq = 100;
  ts->tv_sec = (int64_t)(ticks / freq);
  ts->tv_nsec = (int64_t)(((ticks % freq) * 1000000000ULL) / freq);
}

/* Wall clock, from the CMOS. Sub-second precision comes from the PIT: the
 * RTC only resolves to a second, and a clock that reported .000000000 every
 * time would break anything measuring short intervals with it.
 *
 * Falls back to uptime when the RTC is unreadable, which is the old
 * behaviour , wrong, but wrong in a way callers already tolerate, and better
 * than reporting 1970 with a straight face. */
static void realtime_timespec(struct linux_timespec *ts) {
  u64 epoch = rtc_unix_epoch();
  if (!epoch) {
    monotonic_timespec(ts);
    return;
  }

  u64 freq = pit_get_freq();
  if (!freq)
    freq = 100;
  ts->tv_sec = (int64_t)epoch;
  ts->tv_nsec = (int64_t)(((pit_ticks() % freq) * 1000000000ULL) / freq);
}

#define CLOCK_ID_REALTIME 0
#define CLOCK_ID_MONOTONIC 1

static long sys_clock_gettime(long clock_id, struct linux_timespec *ts) {
  if (!ts || !user_buffer_ok(ts, sizeof(*ts), 1))
    return -1;
  if (clock_id != CLOCK_ID_REALTIME && clock_id != CLOCK_ID_MONOTONIC)
    return -1;

  /* These were the same clock until now, which meant CLOCK_REALTIME reported
   * seconds since boot and every program believed it was 1970. */
  if (clock_id == CLOCK_ID_MONOTONIC)
    monotonic_timespec(ts);
  else
    realtime_timespec(ts);
  return 0;
}

static long sys_gettimeofday(struct linux_timeval *tv, void *tz) {
  (void)tz;
  if (!tv)
    return 0;
  if (!user_buffer_ok(tv, sizeof(*tv), 1))
    return -1;
  /* gettimeofday is wall clock by definition, never uptime. */
  struct linux_timespec ts;
  realtime_timespec(&ts);
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / 1000;
  return 0;
}

static long sys_nanosleep(const struct linux_timespec *req,
                          struct linux_timespec *rem) {
  if (!req || !user_buffer_ok(req, sizeof(*req), 0))
    return -1;
  if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL)
    return -1;
  if (rem && !user_buffer_ok(rem, sizeof(*rem), 1))
    return -1;

  u64 freq = pit_get_freq();
  if (!freq)
    freq = 100;
  u64 ticks = (u64)req->tv_sec * freq;
  ticks += ((u64)req->tv_nsec * freq + 999999999ULL) / 1000000000ULL;
  if (ticks == 0 && (req->tv_sec || req->tv_nsec))
    ticks = 1;
  if (ticks)
    task_sleep_ticks(ticks);
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

static long sys_fstatat(int dirfd, const char *path, struct linux_kstat *out,
                        int flags) {
  if (!out)
    return -1;
  if ((flags & AT_EMPTY_PATH) && path && !path[0] && dirfd >= 0)
    return sys_fstat(dirfd, out);
  if (!path)
    return -1;
  if (dirfd != AT_FDCWD && path[0] != '/')
    return -1;
  return sys_stat(path, out);
}

static long sys_set_tid_address(u64 clear_tid_addr) {
  (void)clear_tid_addr;
  struct task *t = task_current();
  return t ? t->pid : -1;
}

static long sys_linux_futex(u32 *addr, int op, u32 val,
                            const struct linux_timespec *timeout) {
  (void)timeout;
  switch (op & FUTEX_CMD_MASK) {
  case FUTEX_WAIT:
    return sys_futex_wait(addr, val);
  case FUTEX_WAKE:
    return sys_futex_wake(addr);
  default:
    return -1;
  }
}

static long sys_mkdir(const char *path) {
  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return vfs_mkdir(resolved);
}

static long sys_chdir(const char *path) {
  struct task *t = task_current();
  if (!t || !t->files || !path)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  /* Existence check by stat rather than by reading an entry: a directory
   * whose first name is long would not fit in a probe buffer, and an
   * empty directory has no entry to read at all. */
  struct vfs_stat st;
  if (vfs_stat(resolved, &st) != 0 || st.type != VFS_NODE_DIRECTORY)
    return -1;

  usize i = 0;
  while (i < TASK_CWD_MAX - 1 && resolved[i]) {
    t->files->cwd[i] = resolved[i];
    i++;
  }
  t->files->cwd[i] = 0;
  return 0;
}

static long sys_getcwd(char *buf, usize size) {
  struct task *t = task_current();
  if (!t || !t->files || !buf || size == 0)
    return -1;
  if (!user_buffer_ok(buf, size, 1))
    return -1;

  usize i = 0;
  while (i + 1 < size && t->files->cwd[i]) {
    buf[i] = t->files->cwd[i];
    i++;
  }
  buf[i] = 0;

  if (t->files->cwd[i])
    return -1;

  return (long)(uintptr_t)buf;
}

/* Userspace-visible proc_info layout. MUST match struct proc_info in
 * userspace/lib/syscall.h byte-for-byte. */
struct proc_info_user {
  u64 ticks_run;
  int pid;
  int parent_pid;
  int state;
  char name[16];
};

struct mem_stats_user {
  u64 total_frames;
  u64 used_frames;
  u64 frame_size;
};

_Static_assert(sizeof(struct fb_info) == 32,
               "fb_info userspace ABI must be four u64 fields");
_Static_assert(sizeof(struct proc_info_user) == 40,
               "proc_info_user userspace ABI must stay compact");
_Static_assert(offsetof(struct proc_info_user, ticks_run) == 0,
               "proc_info_user.ticks_run offset is userspace ABI");
_Static_assert(offsetof(struct proc_info_user, pid) == 8,
               "proc_info_user.pid offset is userspace ABI");
_Static_assert(offsetof(struct proc_info_user, name) == 20,
               "proc_info_user.name offset is userspace ABI");
_Static_assert(sizeof(struct mem_stats_user) == 24,
               "mem_stats_user userspace ABI must be three u64 fields");

static long sys_proc_list(struct proc_info_user *out, long max) {
  if (!out || max <= 0)
    return -1;
  if (max > 64)
    max = 64; /* hard cap to bound stack snap below */
  struct task_snap snap[64];
  int n = sched_snapshot(snap, (int)max);
  for (int i = 0; i < n; i++) {
    out[i].ticks_run = snap[i].ticks_run;
    out[i].pid = snap[i].pid;
    out[i].parent_pid = snap[i].parent_pid;
    out[i].state = snap[i].state;
    for (usize k = 0; k < sizeof(out[i].name); k++) {
      out[i].name[k] = snap[i].name[k];
    }
  }
  return n;
}

static long sys_mem_stats(struct mem_stats_user *out) {
  if (!out)
    return -1;
  out->total_frames = pmm_usable_frames();
  out->used_frames = pmm_used_frames();
  out->frame_size = 4096;
  return 0;
}

/* Both of these write into user memory, so validate before the netmon
 * layer copies: it runs with the ring lock held and interrupts off, which
 * is no place to take a page fault. user_buffer_ok() faults the pages in
 * up front, so the copy underneath the lock only touches present pages. */
static long sys_net_stats(struct netmon_stats_user *out) {
  if (!user_buffer_ok(out, sizeof(*out), 1))
    return -1;
  return netmon_read_stats(out);
}

static long sys_net_capture(u64 *cursor, struct netmon_frame_user *out,
                            long max) {
  if (max <= 0)
    return -1;
  if (max > NETMON_CAPTURE_BATCH)
    max = NETMON_CAPTURE_BATCH;
  if (!user_buffer_ok(cursor, sizeof(*cursor), 1))
    return -1;
  if (!user_buffer_ok(out, (u64)max * sizeof(*out), 1))
    return -1;
  return netmon_read_frames(cursor, out, max);
}

/* One echo request, one reply, one round trip. The arguments are copied
 * out of user memory before icmp_ping runs because it yields while it
 * waits: leaving the request struct mapped and re-read across a yield
 * would let another thread in the same process change the destination
 * after the packet had already gone. */
/* ---------------- BSD sockets, at the Linux numbers --------------------
 *
 * musl's socket(), bind(), sendto() and recvfrom() issue syscalls 41, 49,
 * 44 and 45 directly. Answering on those numbers is the whole point: it
 * means ported code and musl's own socket API work untouched, rather than
 * every caller needing a TOS-specific shim.
 *
 * Sockets live in the same fd table as files so close() needs no special
 * case. A slot holds one or the other, never both. */

static int sock_fd_alloc(struct task *t, struct socket *sock) {
  for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
    struct task_fd *slot = task_fd_slot(t, fd);
    if (!slot || slot->type != TASK_FD_UNUSED)
      continue;
    task_fd_clear(slot);
    slot->type = TASK_FD_SOCKET;
    slot->socket = sock;
    return fd;
  }
  return -1;
}

static struct socket *sock_from_fd(struct task *t, int fd) {
  if (!t || fd < 3 || fd >= TASK_MAX_FDS)
    return NULL;
  return task_fd_socket(t, fd);
}

static long sys_socket(int domain, int type, int protocol) {
  struct task *t = task_current();
  if (!t)
    return -1;
  if (domain != AF_INET)
    return -1;
  /* UDP only for now. Reporting failure beats handing back an fd that
   * silently drops everything written to it. */
  if (type != SOCK_DGRAM)
    return -1;
  if (protocol != 0 && protocol != (int)IPPROTO_UDP)
    return -1;

  struct socket *sock = socket_create(type, IPPROTO_UDP);
  if (!sock)
    return -1;

  int fd = sock_fd_alloc(t, sock);
  if (fd < 0) {
    socket_close(sock);
    return -1;
  }
  return fd;
}

static long sys_bind(int fd, const struct sockaddr_in *addr, u32 addrlen) {
  struct task *t = task_current();
  struct socket *sock = sock_from_fd(t, fd);
  if (!sock || !addr || addrlen < sizeof(*addr))
    return -1;
  if (!user_buffer_ok(addr, sizeof(*addr), 0))
    return -1;

  struct sockaddr_in local;
  memcpy(&local, addr, sizeof(local));
  if (local.family != AF_INET)
    return -1;

  /* Port 0 means "pick one", which is what a client that only ever sends
   * asks for. Without it the reply has no port to come back to. */
  if (local.port == 0) {
    local.port = socket_alloc_ephemeral_port();
    if (local.port == 0)
      return -1;
  }

  return socket_bind(sock, &local);
}

static long sys_sendto(int fd, const void *buf, usize len, int flags,
                       const struct sockaddr_in *dest, u32 addrlen) {
  (void)flags;
  struct task *t = task_current();
  struct socket *sock = sock_from_fd(t, fd);
  if (!sock || !buf || !dest || addrlen < sizeof(*dest))
    return -1;
  if (!user_buffer_ok(buf, len, 0) || !user_buffer_ok(dest, sizeof(*dest), 0))
    return -1;

  struct sockaddr_in remote;
  memcpy(&remote, dest, sizeof(remote));
  if (remote.family != AF_INET)
    return -1;

  /* An unbound sender still needs a source port for the reply. Binding
   * lazily here is what makes the send-then-receive pattern work. */
  if (sock->local.port == 0) {
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.family = AF_INET;
    local.port = socket_alloc_ephemeral_port();
    if (local.port == 0 || socket_bind(sock, &local) != 0)
      return -1;
  }

  return socket_sendto(sock, buf, len, &remote);
}

static long sys_recvfrom(int fd, void *buf, usize len, int flags,
                         struct sockaddr_in *src, u32 *addrlen) {
  (void)flags;
  struct task *t = task_current();
  struct socket *sock = sock_from_fd(t, fd);
  if (!sock || !buf)
    return -1;
  if (!user_buffer_ok(buf, len, 1))
    return -1;
  if (src && !user_buffer_ok(src, sizeof(*src), 1))
    return -1;
  if (addrlen && !user_buffer_ok(addrlen, sizeof(*addrlen), 1))
    return -1;

  long n = socket_recvfrom(sock, buf, len, src);
  if (n > 0 && addrlen)
    *addrlen = sizeof(*src);
  return n;
}

static long sys_net_ping(struct net_ping_user *req) {
  if (!user_buffer_ok(req, sizeof(*req), 1))
    return -1;

  struct net_ping_user local;
  memcpy(&local, req, sizeof(local));

  if (local.timeout_ms == 0 || local.timeout_ms > 60000u)
    return -1;

  u32 rtt_ms = 0;
  long rc =
      icmp_ping(local.dst, local.ident, local.seq, local.timeout_ms, &rtt_ms);
  if (rc == 0)
    req->rtt_ms = rtt_ms;
  return rc;
}

static long sys_arch_prctl(long code, u64 addr) {
  switch (code) {
  case ARCH_SET_FS:
    if (addr >= USER_VA_MAX)
      return -1;
    return task_set_fs_base(addr);
  case ARCH_GET_FS:
    if (!user_buffer_ok((void *)(uintptr_t)addr, sizeof(u64), 1))
      return -1;
    *(u64 *)(uintptr_t)addr = task_get_fs_base();
    return 0;
  default:
    return -1;
  }
}

static long sys_thread_create(u64 entry, u64 user_stack, u64 u_arg) {
  struct task *t = task_spawn_thread(entry, user_stack);
  if (!t)
    return -1;
  t->context->user_arg = u_arg;
  return t->pid;
}

static long sys_thread_exit(void) {
  struct task *t = task_current();
  if (!t || !t->vm || !t->vm->pml4_ref_count)
    return -1;

  /* Threads share one PML4, so the last one out frees it. The decrement
   * must be atomic: two threads exiting concurrently would otherwise both
   * read the same count and either double-free or leak the address space. */
  int new_count =
      __atomic_sub_fetch(t->vm->pml4_ref_count, 1, __ATOMIC_ACQ_REL);

  if (new_count <= 0) {
    kfree(t->vm->pml4_ref_count);
    t->vm->pml4_ref_count = 0;
    task_exit(0); /* tears down the address space too */
  } else {
    /* Detach from the shared PML4 first , task_exit_thread must not
     * reap an address space its siblings are still running on. */
    t->vm->user_pml4 = 0;
    t->vm->pml4_ref_count = 0;
    task_exit_thread();
  }

  return 0;
}

static long sys_thread_join(long tid) {
  struct task *target = task_find((int)tid);
  if (!target)
    return -1;

  if (target->state == TASK_ZOMBIE) {
    return 0;
  }

  task_block((int)tid);
  return 0;
}

/* Block until *addr changes away from `expected`.
 *
 * The comparison and the block have to look atomic to userspace: if the value
 * changed between the caller's own check and ours, returning immediately is
 * what stops the caller sleeping through a wake it already missed. */
static long sys_futex_wait(u32 *addr, u32 expected) {
  struct task *t = task_current();
  if (!task_pml4(t))
    return -1;
  if (!addr)
    return -1;

  if ((u64)addr >= USER_VA_MAX)
    return -1;

  /* Futexes key on the physical frame, so two tasks with the same page
   * mapped at different VAs still queue on the same futex. */
  u64 phys = vmm_translate_in(t->vm->user_pml4, (u64)addr);
  if (!phys)
    return -1;
  phys &= VMM_ADDR_MASK;

  if (*addr != expected) {
    return -1;
  }

  t->futex_addr = phys;
  task_block(0); /* 0 = indefinite; only a futex wake releases us */
  return 0;
}

/* Wake exactly one waiter. Callers needing wake-all must loop. */
static long sys_futex_wake(u32 *addr) {
  struct task *me = task_current();
  if (!task_pml4(me))
    return -1;
  if (!addr)
    return -1;

  u64 phys = vmm_translate_in(me->vm->user_pml4, (u64)addr);
  if (!phys)
    return -1;
  phys &= VMM_ADDR_MASK;

  return task_wake_futex(phys);
}

long syscall_dispatch(struct syscall_frame *f) {
  long num = (long)f->rax;
  long a1 = (long)f->rdi;
  long a2 = (long)f->rsi;
  long a3 = (long)f->rdx;
  /* r10, not rcx: the syscall instruction clobbers rcx with the return RIP,
   * so the ABI substitutes r10 for the fourth argument. */
  long a4 = (long)f->r10;
  /* Six-argument calls (sendto, recvfrom) need these two; everything else
   * simply ignores them. */
  long a5 = (long)f->r8;
  long a6 = (long)f->r9;

  long ret = -1;
  switch (num) {
  case SYS_WRITE:
    ret = sys_write((uintptr_t)a1, (const void *)(uintptr_t)a2, (uintptr_t)a3);
    break;
  case SYS_READV:
    ret = sys_readv((int)a1, (const struct linux_iovec *)(uintptr_t)a2, a3);
    break;
  case SYS_WRITEV:
    ret = sys_writev((int)a1, (const struct linux_iovec *)(uintptr_t)a2, a3);
    break;
  case SYS_POLL:
    ret = sys_poll((struct linux_pollfd *)(uintptr_t)a1, a2, a3);
    break;
  case SYS_READ:
    ret = sys_read((uintptr_t)a1, (void *)(uintptr_t)a2, (uintptr_t)a3);
    break;
  case SYS_OPEN:
    ret = sys_open((const char *)(uintptr_t)a1, (uintptr_t)a2);
    break;
  case SYS_CLOSE:
    ret = sys_close((uintptr_t)a1);
    break;
  case SYS_LSEEK:
    ret = sys_lseek((uintptr_t)a1, (uintptr_t)a2, (uintptr_t)a3);
    break;
  case SYS_MMAP:
    ret = sys_mmap((u64)a1, a2, (int)a3, (int)a4);
    break;
  case SYS_MPROTECT:
    ret = sys_mprotect((u64)a1, a2, (int)a3);
    break;
  case SYS_MUNMAP:
    ret = sys_munmap((u64)a1, a2);
    break;
  case SYS_BRK:
    ret = sys_brk((u64)(uintptr_t)a1);
    break;
  case SYS_RT_SIGACTION:
    ret = sys_rt_sigaction(
        (int)a1, (const struct task_signal_action *)(uintptr_t)a2,
        (struct task_signal_action *)(uintptr_t)a3, (usize)a4);
    break;
  case SYS_STAT:
    ret = sys_stat((const char *)(uintptr_t)a1,
                   (struct linux_kstat *)(uintptr_t)a2);
    break;
  case SYS_FSTAT:
    ret = sys_fstat((int)a1, (struct linux_kstat *)(uintptr_t)a2);
    break;
  case SYS_STAT_RAW:
    ret = sys_stat_raw((const char *)(uintptr_t)a1,
                       (struct stat_user *)(uintptr_t)a2);
    break;
  case SYS_FSTAT_RAW:
    ret = sys_fstat_raw((int)a1, (struct stat_user *)(uintptr_t)a2);
    break;
  case SYS_FSTATAT:
    ret = sys_fstatat((int)a1, (const char *)(uintptr_t)a2,
                      (struct linux_kstat *)(uintptr_t)a3, (int)a4);
    break;
  /* 217 is getdents64 and nothing else. It used to also carry TOS's
   * index-based walk, disambiguated by testing whether the first argument
   * looked like an fd , which is a guess about a pointer value, and exactly
   * the kind of overload that makes a libc built for Linux misbehave. The
   * TOS form now has its own number. */
  case SYS_READDIR:
    ret = sys_getdents64((int)a1, (void *)(uintptr_t)a2, (usize)a3);
    break;
  case SYS_READDIR_INDEX:
    ret = sys_readdir((u32 *)(uintptr_t)a1, (char *)(uintptr_t)a2, a3);
    break;
  case SYS_READDIR_PATH:
    ret = sys_readdir_path((const char *)(uintptr_t)a1, (u32 *)(uintptr_t)a2,
                           (char *)(uintptr_t)a3, (usize)a4);
    break;
  case SYS_SET_TID_ADDRESS:
    if ((u64)a2 >= USER_VA_MIN && (u64)a3 >= USER_VA_MIN && a4 > 0) {
      ret = sys_readdir_path((const char *)(uintptr_t)a1, (u32 *)(uintptr_t)a2,
                             (char *)(uintptr_t)a3, (usize)a4);
    } else {
      ret = sys_set_tid_address((u64)(uintptr_t)a1);
    }
    break;
  case SYS_CHDIR:
    ret = sys_chdir((const char *)(uintptr_t)a1);
    break;

  case SYS_GETCWD:
    ret = sys_getcwd((char *)(uintptr_t)a1, (usize)a2);
    break;
  case SYS_IOCTL:
    ret = sys_ioctl((int)a1, a2, (void *)(uintptr_t)a3);
    break;
  case SYS_FCNTL:
    ret = sys_fcntl((int)a1, (int)a2, a3);
    break;
  case SYS_CLOCK_GETTIME:
    ret = sys_clock_gettime(a1, (struct linux_timespec *)(uintptr_t)a2);
    break;
  case SYS_GETTIMEOFDAY:
    ret = sys_gettimeofday((struct linux_timeval *)(uintptr_t)a1,
                           (void *)(uintptr_t)a2);
    break;
  case SYS_NANOSLEEP:
    ret = sys_nanosleep((const struct linux_timespec *)(uintptr_t)a1,
                        (struct linux_timespec *)(uintptr_t)a2);
    break;
  case SYS_LINUX_GETPID:
    ret = sys_get_pid();
    break;
  case SYS_UNAME:
    ret = sys_uname((struct linux_utsname *)(uintptr_t)a1);
    break;
  case SYS_EXIT:
    ret = sys_exit((uintptr_t)a1);
    break;
  case SYS_EXIT_GROUP:
    ret = sys_exit((uintptr_t)a1);
    break;
  case SYS_YIELD:
    ret = sys_yield();
    break;
  case SYS_MSG_GET:
    ret = sys_msg_get((struct msg *)(uintptr_t)a1);
    break;
  case SYS_MSG_PEEK:
    ret = sys_msg_peek((struct msg *)(uintptr_t)a1);
    break;
  case SYS_MOUSE_POS:
    ret = sys_mouse_pos((int32_t *)(uintptr_t)a1, (int32_t *)(uintptr_t)a2,
                        (u8 *)(uintptr_t)a3);
    break;
  case SYS_CON_WRITE:
    ret = sys_con_write((const char *)(uintptr_t)a1, (uintptr_t)a2);
    break;
  case SYS_CON_ZOOM:
    ret = sys_con_zoom(a1);
    break;
  case SYS_AUDIO_OPEN:
    ret = sys_audio_open(a1, a2, a3);
    break;
  case SYS_AUDIO_WRITE:
    ret = sys_audio_write((const void *)(uintptr_t)a1, a2);
    break;
  case SYS_AUDIO_STATUS:
    ret = sys_audio_status((struct audio_status_user *)(uintptr_t)a1);
    break;
  case SYS_AUDIO_DRAIN:
    ret = sys_audio_drain();
    break;
  case SYS_AUDIO_CLOSE:
    ret = sys_audio_close();
    break;
  case SYS_AUDIO_SET_VOLUME:
    ret = sys_audio_set_volume(a1);
    break;
  case SYS_AUDIO_PAUSE:
    ret = sys_audio_pause();
    break;
  case SYS_AUDIO_RESUME:
    ret = sys_audio_resume();
    break;
  case SYS_ARCH_PRCTL:
    ret = sys_arch_prctl(a1, (u64)(uintptr_t)a2);
    break;
  case SYS_CON_CLEAR:
    ret = sys_con_clear();
    break;
  case SYS_SLEEP_TICKS:
    ret = sys_sleep_ticks((uintptr_t)a1);
    break;
    /* SYS_GET_PID is an alias for SYS_LINUX_GETPID (39) and dispatches
     * through that case; a second label would be a duplicate. */
  case SYS_IPC_SEND:
    ret = sys_ipc_send((uintptr_t)a1, (const struct ipc_msg *)(uintptr_t)a2);
    break;
  case SYS_IPC_RECV:
    ret = sys_ipc_recv((struct ipc_msg *)(uintptr_t)a1);
    break;
  case SYS_SHMEM_SHARE:
    ret = sys_shmem_share((uintptr_t)a1, (u64)(uintptr_t)a2, (uintptr_t)a3,
                          (u64 *)(uintptr_t)a4);
    break;
  case SYS_SHMEM_UNSHARE:
    ret = sys_shmem_unshare((uintptr_t)a1, (u64)(uintptr_t)a2, (uintptr_t)a3);
    break;
  case SYS_WM_REGISTER:
    ret = sys_wm_register();
    break;
  case SYS_WM_PID:
    ret = sys_wm_pid();
    break;
  case SYS_TTY_DRAIN:
    ret = sys_tty_drain((long)a1, (char *)(uintptr_t)a2, (uintptr_t)a3);
    break;
  case SYS_TTY_INJECT:
    /* Push the ASCII char into channel a1's input ring. */
    tty_inject_input_ch((int)a1, (char)a2);
    ret = 0;
    break;
  case SYS_TTY_READ_INPUT:
    ret = sys_tty_read_input((char *)(uintptr_t)a1, (uintptr_t)a2);
    break;
  case SYS_TTY_ALLOC:
    ret = sys_tty_alloc();
    break;
  case SYS_TTY_FREE:
    ret = sys_tty_free((long)a1);
    break;
  case SYS_TTY_SPAWN:
    ret = sys_tty_spawn((const char *)(uintptr_t)a1,
                        (char *const *)(uintptr_t)a2, (long)a3);
    break;
  case SYS_FB_INFO:
    ret = sys_fb_info((struct fb_info *)(uintptr_t)a1);
    break;
  case SYS_FB_MAP:
    ret = sys_fb_map();
    break;
  case SYS_FB_DAMAGE:
    framebuffer_mark_damage((u32)a1, (u32)a2, (u32)a3, (u32)a4);
    ret = 0;
    break;
  case SYS_FB_PRESENT:
    ret = sys_fb_present((const void *)(uintptr_t)a1, (u64)a2,
                         (const struct fb_rect *)(uintptr_t)a3, (u64)a4);
    break;
  case SYS_FB_REGISTER:
    ret = sys_fb_register((const void *)(uintptr_t)a1, (u64)a2);
    break;
  case SYS_FB_UNREGISTER:
    ret = sys_fb_unregister();
    break;
  case SYS_KBD_POLL:
    ret = sys_kbd_poll((int *)(uintptr_t)a1, (u16 *)(uintptr_t)a2);
    break;
  case SYS_GET_TICKS:
    ret = sys_get_ticks();
    break;
  case SYS_UNLINK:
    ret = sys_unlink((const char *)(uintptr_t)a1);
    break;
  case SYS_RMDIR:
    ret = sys_rmdir((const char *)(uintptr_t)a1);
    break;
  case SYS_MKDIR:
    ret = sys_mkdir((const char *)(uintptr_t)a1);
    break;
  case SYS_EXEC:
    ret = sys_exec((const char *)(uintptr_t)a1, (char *const *)(uintptr_t)a2);
    break;
  case SYS_SPAWN:
    ret = sys_spawn((const char *)(uintptr_t)a1, (char *const *)(uintptr_t)a2);
    break;
  case SYS_KILL:
    ret = sys_kill((uintptr_t)a1, (uintptr_t)a2);
    break;
  case SYS_SHUTDOWN:
    ret = sys_shutdown((uintptr_t)a1, (const char *)(uintptr_t)a2);
    break;
  case SYS_REBOOT:
    ret = sys_reboot((uintptr_t)a1);
    break;
  case SYS_PROC_LIST:
    ret = sys_proc_list((struct proc_info_user *)(uintptr_t)a1, (long)a2);
    break;
  case SYS_MEM_STATS:
    ret = sys_mem_stats((struct mem_stats_user *)(uintptr_t)a1);
    break;
  case SYS_CON_PUSH:
    ret = sys_con_push();
    break;
  case SYS_CON_POP:
    ret = sys_con_pop();
    break;
  case SYS_THREAD_CREATE:
    ret = sys_thread_create((uintptr_t)a1, (uintptr_t)a2, (uintptr_t)a3);
    break;
  case SYS_THREAD_EXIT:
    ret = sys_thread_exit();
    break;
  case SYS_THREAD_JOIN:
    if ((u64)a1 >= USER_VA_MIN) {
      ret = sys_linux_futex((u32 *)(uintptr_t)a1, (int)a2, (u32)a3,
                            (const struct linux_timespec *)(uintptr_t)a4);
    } else {
      ret = sys_thread_join((uintptr_t)a1);
    }
    break;
  case SYS_FUTEX_WAIT:
    ret = sys_futex_wait((u32 *)(uintptr_t)a1, (u32)a2);
    break;
  case SYS_FUTEX_WAKE:
    ret = sys_futex_wake((u32 *)(uintptr_t)a1);
    break;
  case SYS_NET_STATS:
    ret = sys_net_stats((struct netmon_stats_user *)(uintptr_t)a1);
    break;
  case SYS_NET_CAPTURE:
    ret = sys_net_capture((u64 *)(uintptr_t)a1,
                          (struct netmon_frame_user *)(uintptr_t)a2, (long)a3);
    break;
  case SYS_NET_PING:
    ret = sys_net_ping((struct net_ping_user *)(uintptr_t)a1);
    break;
  case SYS_SOCKET:
    ret = sys_socket((int)a1, (int)a2, (int)a3);
    break;
  case SYS_BIND:
    ret = sys_bind((int)a1, (const struct sockaddr_in *)(uintptr_t)a2, (u32)a3);
    break;
  case SYS_SENDTO:
    ret = sys_sendto((int)a1, (const void *)(uintptr_t)a2, (usize)a3, (int)a4,
                     (const struct sockaddr_in *)(uintptr_t)a5, (u32)a6);
    break;
  case SYS_RECVFROM:
    ret =
        sys_recvfrom((int)a1, (void *)(uintptr_t)a2, (usize)a3, (int)a4,
                     (struct sockaddr_in *)(uintptr_t)a5, (u32 *)(uintptr_t)a6);
    break;
  default:
    log_write_hex("unknown syscall =", num, KERNEL, LOG_ERROR);
  }
  f->rax = (u64)ret;
  if (syscall_prepare_return(f) != 0) {
    log_write("syscall: rejected invalid ring-3 return frame", KERNEL,
              LOG_ERROR);
    task_exit(-11);
  }
  return ret;
}
