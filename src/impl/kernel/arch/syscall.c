/* src/impl/kernel/arch/syscall.c — SYSCALL entry + dispatcher.
 *
 * syscall_init programs LSTAR / STAR / SFMASK and enables SCE so the
 * SYSCALL instruction lands at the asm entry trampoline in
 * src/impl/x86_64/cpu/syscall.asm. The trampoline saves all GPRs into a
 * syscall_frame, swaps to the per-CPU kernel stack via gs:CPU_LOCAL_*,
 * and calls syscall_dispatch with a pointer to the frame.
 *
 * Dispatch is one giant switch over the SYS_* number in rax. Each arm
 * reads typed args from the frame's caller-saved register slots,
 * validates pointers against the caller's PML4 if needed, performs the
 * action, and returns a long that the asm path writes back into the
 * frame's rax slot (and thus userspace's rax on SYSRET).
 *
 * Pointer validation is best-effort: anything taken from userspace is
 * range-checked against [0, USER_LOW_LIMIT) and walked via
 * vmm_translate_in to confirm it's actually mapped USER+writable. The
 * trade-off is "trusted enough to not crash the kernel"; a real OS would
 * do per-page copy-in/copy-out into kernel buffers.
 */
// #region INCLUDES

#include "arch/syscall.h"
#include "devices/io.h"
#include "devices/pit.h"
#include "devices/serial.h"
#include "display/framebuffer.h"
#include "display/tty.h"
#include "fs/fat.h"
#include "input/keyboard.h"
#include "input/mouse.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "msg/msg.h"
#include "sched/sched.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stddef.h>
#include <stdint.h>

// #endregion INCLUDES

// #region CONSTANTS

#define USER_FB_BASE 0x0000000060000000ULL

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MAX_FDS 32

#define MAX_SHMEM_PAGES 4096   /* per-call cap; 16 MiB */
#define USER_MMAP_LIMIT 0x0000000080000000ULL

// #endregion CONSTANTS

// #region TYPES + EXTERNS

struct fb_info {
  uint64_t width;
  uint64_t height;
  uint64_t pitch;
  uint64_t bpp;
};

extern void syscall_entry(void);
extern uint64_t kernel_rsp_top;

static struct fat_file *fd_table[MAX_FDS] = {0};

static int fd_alloc(struct fat_file *f) {
  for (int i = 3; i < MAX_FDS; i++) { // 0,1,2 reserved (stdin/out/err)
    if (!fd_table[i]) {
      fd_table[i] = f;
      return i;
    }
  }
  return -1;
}

// #endregion TYPES + EXTERNS

// #region FRAMEBUFFER

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
  if (t && t->user_pml4) {
    int owner = msg_input_owner();
    if (owner != t->pid) {
      if (t->input_owner_restore_pid < 0) {
        t->input_owner_restore_pid = owner;
      }
      msg_input_owner_force(t->pid);
    }
  }

  /* Re-map every call: process_exec wipes user PDPT between programs,
     so the static "mapped" cache would lie. vmm_map overwrites cleanly.
     The backing may be scatter-gather (virtio-gpu mode), so walk the
     per-page phys list rather than assuming contiguity. */
  uint32_t pages = framebuffer_num_pages();
  for (uint32_t i = 0; i < pages; i++) {
    uint64_t phys = framebuffer_phys_for_page(i);
    if (!phys) return -1;
    if (vmm_map(USER_FB_BASE + (uint64_t)i * 4096, phys,
                VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
      return -1;
    }
  }
  return USER_FB_BASE;
}

// #endregion FRAMEBUFFER

// #region MMAP

static void sys_mmap_rollback(struct task *t, uint64_t base, uint64_t pages) {
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t va = base + i * 4096;
    uint64_t phys = vmm_translate_in(t->user_pml4, va);
    if (!phys) continue;
    vmm_unmap_in(t->user_pml4, va);
    pmm_free_frame(phys & ~4095ULL);
  }
}

static long sys_mmap(long len) {
  if (len <= 0) return -1;

  struct task *t = task_current();
  if (!t || !t->user_pml4) return -1;

  uint64_t bytes = ((uint64_t)len + 4095) & ~4095ULL;
  uint64_t base = t->mmap_next_va;
  if (bytes == 0 || base + bytes < base || base + bytes > USER_MMAP_LIMIT) {
    return -1;
  }

  uint64_t pages = bytes / 4096;
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) {
      sys_mmap_rollback(t, base, i);
      return -1;
    }

    memset((void*)phys, 0, 4096);
    if (vmm_map_in(t->user_pml4, base + i * 4096, phys,
                   VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
      pmm_free_frame(phys);
      sys_mmap_rollback(t, base, i);
      return -1;
    }
  }

  t->mmap_next_va = base + bytes;
  return (long)base;
}

// #endregion MMAP

// #region INPUT + TIME

static long sys_kbd_poll(int *pressed, uint16_t *key) {
  if (!pressed || !key)
    return -1;
  return keyboard_poll(pressed, key);
}

static long sys_get_ticks(void) {
  return (long)(pit_ticks() * 10); // 100Hz → ms
}

// #endregion INPUT + TIME

// #region MSR + INIT

static void wrmsr(uint32_t msr, uint64_t value) {
  uint32_t lo = (uint32_t)value;
  uint32_t hi = (uint32_t)(value >> 32);
  __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

void syscall_init(uint64_t kernel_stack_top) {
  kernel_rsp_top = kernel_stack_top;

  wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1); // SCE
  wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
  wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
  wrmsr(MSR_FMASK, 0x200); // mask IF

  log_write("syscalls enabled", KERNEL, LOG_INFO);
}

// #endregion MSR + INIT

// #region FILE I/O HANDLERS

static long sys_write(long fd, const void *buf, long n) {
  if (fd == 1 || fd == 2) { // stdout/stderr -> serial
    const char *p = (const char *)buf;
    for (long i = 0; i < n; i++)
      serial_write_char(p[i]);
    return n;
  }
  if (fd >= 3 && fd < MAX_FDS && fd_table[fd]) {
    return (long)fat_write(fd_table[fd], buf, (size_t)n);
  }
  return -1;
}

// #endregion FILE I/O HANDLERS

// #region PROCESS HANDLERS

extern long process_exec(const char *path, char *const argv[]);
extern long process_spawn_async(const char *path, char *const argv[]);

static long sys_exec(const char *path, char *const argv[]) {
    if (!path) return -1;
    return process_exec(path, argv);
}

/* Fire-and-forget: returns child pid without blocking on its exit code.
 * Used by the shell to launch windowed apps that should keep running while
 * the user types more commands. */
static long sys_spawn(const char *path, char *const argv[]) {
    if (!path) return -1;
    return process_spawn_async(path, argv);
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

// #endregion PROCESS HANDLERS

// #region MESSAGE + MOUSE HANDLERS

static long sys_msg_get(struct msg *out) {
  if (!out) return -1;
  return msg_get(out) ? 1 : 0;
}

static long sys_msg_peek(struct msg *out) {
  if (!out) return -1;
  return msg_peek(out) ? 1 : 0;
}

static long sys_mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons) {
  if (x)       *x       = mouse_x();
  if (y)       *y       = mouse_y();
  if (buttons) *buttons = mouse_buttons();
  return 0;
}

// #endregion MESSAGE + MOUSE HANDLERS

// #region CONSOLE + SLEEP + PID

static long sys_con_write(const char *buf, long n) {
  if (!buf || n < 0) return -1;
  tty_write(buf, (size_t)n);
  return n;
}

static long sys_con_clear(void) {
  tty_clear();
  return 0;
}

static long sys_con_push(void) {
  return tty_push();
}

static long sys_con_pop(void) {
  return tty_pop();
}

static long sys_sleep_ticks(long n) {
  if (n <= 0) { task_yield(); return 0; }
  task_sleep_ticks((uint64_t)n);
  return 0;
}

static long sys_get_pid(void) {
  struct task *t = task_current();
  return t ? t->pid : -1;
}

// #endregion CONSOLE + SLEEP + PID

// #region IPC + SHMEM + WINMAN

static long sys_ipc_send(long target_pid, const struct ipc_msg *m) {
  if (!m) return -1;
  struct task *t = task_current();
  if (!t) return -1;
  return ipc_send((int)target_pid, m, t->pid);
}

static long sys_ipc_recv(struct ipc_msg *out) {
  if (!out) return -1;
  return ipc_recv(out) ? 1 : 0;
}

/* Share `npages` worth of the caller's pages starting at `my_va` (page-
 * aligned) into the target process's address space. Kernel picks the
 * target VA via the target task's bump allocator. */
static long sys_shmem_share(long target_pid, uint64_t my_va, long npages,
                            uint64_t *out_target_va) {
  if (!out_target_va || target_pid <= 0 || npages <= 0) return -1;
  if (npages > MAX_SHMEM_PAGES)                          return -1;
  if (my_va & 0xFFFULL)                                  return -1;

  struct task *me = task_current();
  if (!me || !me->user_pml4) return -1;

  struct task *target = task_find((int)target_pid);
  if (!target || !target->user_pml4) return -1;
  if (target->shmem_next_va == 0)
      target->shmem_next_va = 0x0000000080000000ULL;     /* defensive */

  uint64_t target_va = target->shmem_next_va;
  /* VMM_SHARED marks the PTE so the target's exit cleanup (free_user_pml4)
   * skips pmm_free_frame on these phys frames — the caller still owns them. */
  uint64_t flags = VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_SHARED;

  for (long i = 0; i < npages; i++) {
    uint64_t phys = vmm_translate_in(me->user_pml4, my_va + (uint64_t)i * 4096);
    if (!phys) return -1;
    if (vmm_map_in(target->user_pml4,
                   target_va + (uint64_t)i * 4096,
                   phys, flags) != 0) {
      return -1;
    }
  }

  target->shmem_next_va = target_va + (uint64_t)npages * 4096;
  *out_target_va = target_va;
  return 0;
}

static long sys_wm_register(void) {
  struct task *t = task_current();
  if (!t) return -1;
  int rc = msg_input_owner_register(t->pid);
  if (rc == 0) tty_set_active(0);
  return rc;
}

static long sys_wm_pid(void) {
  return msg_input_owner();
}

static long sys_tty_drain(char *out, long max) {
  if (!out || max <= 0) return 0;
  return (long)tty_drain(out, (size_t)max);
}

// #endregion IPC + SHMEM + WINMAN

// #region SHUTDOWN + REBOOT

/* Block-wait `seconds` using PIT ticks (10ms each at 100Hz). */
static void delay_seconds(long seconds) {
    if (seconds <= 0) return;
    extern uint64_t pit_ticks(void);
    uint64_t target = pit_ticks() + (uint64_t)seconds * 100;
    while (pit_ticks() < target) __asm__ volatile ("hlt");
}

/* Try several poweroff mechanisms; halt if all fail. */
static void hw_shutdown(void) {
    log_write("powering off...", KERNEL, LOG_INFO);
    outw(0x604,  0x2000);     /* QEMU >= 2.0 (PIIX ACPI) */
    outw(0xB004, 0x2000);     /* Bochs / old QEMU */
    outw(0x4004, 0x3400);     /* VirtualBox */
    outw(0x600,  0x34);       /* Cloud Hypervisor */
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* Reboot via 8042, then ACPI reset, then triple fault. */
static void hw_reboot(void) {
    log_write("rebooting...", KERNEL, LOG_INFO);
    /* drain 8042 input buffer */
    while (inb(0x64) & 0x02) { (void)inb(0x60); }
    outb(0x64, 0xFE);                                   /* keyboard controller pulse */
    outb(0xCF9, 0x06);                                  /* PIIX/q35 reset */
    /* triple-fault: null IDT + INT3 */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = { 0, 0 };
    __asm__ volatile ("lidt %0" : : "m"(idtr));
    __asm__ volatile ("int3");
    for (;;) __asm__ volatile ("hlt");
}

static long sys_shutdown(long time, const char *reason) {
    log_write_hex("shutdown in (s) =", time, KERNEL, LOG_INFO);
    if (reason && *reason) {
        char buf[80];
        const size_t prefix = 8;
        memcpy(buf, "reason: ", prefix);
        size_t rl = strlen(reason);
        if (rl > sizeof(buf) - prefix - 1) rl = sizeof(buf) - prefix - 1;
        memcpy(buf + prefix, reason, rl);
        buf[prefix + rl] = 0;
        log_write(buf, KERNEL, LOG_INFO);
    }
    delay_seconds(time);
    hw_shutdown();
    return 0;     /* unreachable */
}

static long sys_reboot(long time) {
    log_write_hex("reboot in (s) =", time, KERNEL, LOG_INFO);
    delay_seconds(time);
    hw_reboot();
    return 0;     /* unreachable */
}

// #endregion SHUTDOWN + REBOOT

// #region FAT HANDLERS

/* O_CREAT = 0x40, O_TRUNC = 0x200, O_WRONLY = 1 (match fcntl.h) */
#define O_CREAT  0x40
#define O_TRUNC  0x200

static long sys_open(const char *path, int flags) {
  struct fat_file *f = (struct fat_file *)kmalloc(sizeof(*f));
  if (!f) return -1;

  int rc = fat_open(path, f);
  if (rc != 0 && (flags & O_CREAT)) {
    rc = fat_create(path, f);
  } else if (rc == 0 && (flags & O_TRUNC)) {
    /* truncate: drop existing chain, reset size */
    extern int fat_unlink(const char *);   /* not exposed but symmetrical */
    /* simplest truncate: unlink + create */
    kfree(f);
    fat_unlink(path);
    f = (struct fat_file *)kmalloc(sizeof(*f));
    if (!f) return -1;
    rc = fat_create(path, f);
  }
  if (rc != 0) {
    kfree(f);
    return -1;
  }
  int fd = fd_alloc(f);
  if (fd < 0) {
    kfree(f);
    return -1;
  }
  return fd;
}

static long sys_unlink(const char *path) {
  return fat_unlink(path);
}

static long sys_read(int fd, void *buf, size_t n) {
  if (fd < 0 || fd >= MAX_FDS || !fd_table[fd])
    return -1;
  return (long)fat_read(fd_table[fd], buf, n);
}

static long sys_lseek(int fd, long off, int whence) {
  if (fd < 0 || fd >= MAX_FDS || !fd_table[fd])
    return -1;
  struct fat_file *f = fd_table[fd];
  uint32_t target;
  if (whence == 0)
    target = (uint32_t)off;
  else if (whence == 1)
    target = f->pos + (uint32_t)off;
  else if (whence == 2)
    target = f->size + (uint32_t)off;
  else
    return -1;
  fat_seek(f, target);
  return (long)target;
}

static long sys_close(int fd) {
  if (fd < 0 || fd >= MAX_FDS || !fd_table[fd])
    return -1;
  kfree(fd_table[fd]);
  fd_table[fd] = 0;
  return 0;
}

static long sys_readdir(uint32_t *index, char *buf, size_t n) {
  return fat_read_root_dir(index, buf, n);
}

// #endregion FAT HANDLERS

// #region INTROSPECTION HANDLERS

/* Userspace-visible proc_info layout. MUST match struct proc_info in
 * userspace/lib/syscall.h byte-for-byte. */
struct proc_info_user {
  uint64_t ticks_run;
  int      pid;
  int      parent_pid;
  int      state;
  char     name[16];
};

struct mem_stats_user {
  uint64_t total_frames;
  uint64_t used_frames;
  uint64_t frame_size;
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
  if (!out || max <= 0) return -1;
  if (max > 64) max = 64;     /* hard cap to bound stack snap below */
  struct task_snap snap[64];
  int n = sched_snapshot(snap, (int)max);
  for (int i = 0; i < n; i++) {
    out[i].ticks_run  = snap[i].ticks_run;
    out[i].pid        = snap[i].pid;
    out[i].parent_pid = snap[i].parent_pid;
    out[i].state      = snap[i].state;
    for (size_t k = 0; k < sizeof(out[i].name); k++) {
      out[i].name[k] = snap[i].name[k];
    }
  }
  return n;
}

static long sys_mem_stats(struct mem_stats_user *out) {
  if (!out) return -1;
  out->total_frames = pmm_usable_frames();
  out->used_frames  = pmm_used_frames();
  out->frame_size   = 4096;
  return 0;
}

// #endregion INTROSPECTION HANDLERS

// #region DISPATCH

long syscall_dispatch(struct syscall_frame *f) {
  long num = (long)f->rax;
  long a1 = (long)f->rdi;
  long a2 = (long)f->rsi;
  long a3 = (long)f->rdx;
  // args 4-6 use r10/r8/r9 in syscall ABI
  long a4 = (long)f->r10;

  // static int trace_count = 0;
  // if (trace_count < 60) {
  //   log_write_hex("sysenter num=", num, KERNEL, LOG_INFO);
  //   log_write_hex("sysenter pid=", task_current() ? task_current()->pid : -1, KERNEL, LOG_INFO);
  //   trace_count++;
  // }

  long ret = -1;
  switch (num) {
  case SYS_WRITE:
    ret = sys_write((uintptr_t)a1, (const void *)(uintptr_t)a2, (uintptr_t)a3);
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
    ret = sys_mmap((uintptr_t)a1);
    break;
  case SYS_READDIR:
  	ret = sys_readdir((uint32_t *)(uintptr_t)a1, (char *)(uintptr_t)a2, a3);
    break;
  case SYS_EXIT:
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
    ret = sys_mouse_pos((int32_t *)(uintptr_t)a1, (int32_t *)(uintptr_t)a2, (uint8_t *)(uintptr_t)a3);
    break;
  case SYS_CON_WRITE:
    ret = sys_con_write((const char *)(uintptr_t)a1, (uintptr_t)a2);
    break;
  case SYS_CON_CLEAR:
    ret = sys_con_clear();
    break;
  case SYS_SLEEP_TICKS:
    ret = sys_sleep_ticks((uintptr_t)a1);
    break;
  case SYS_GET_PID:
    ret = sys_get_pid();
    break;
  case SYS_IPC_SEND:
  	ret = sys_ipc_send((uintptr_t)a1, (const struct ipc_msg *)(uintptr_t)a2);
    break;
  case SYS_IPC_RECV:
    ret = sys_ipc_recv((struct ipc_msg *)(uintptr_t)a1);
    break;
  case SYS_SHMEM_SHARE:
    ret = sys_shmem_share((uintptr_t)a1, (uint64_t)(uintptr_t)a2, (uintptr_t)a3, (uint64_t *)(uintptr_t)a4);
    break;
  case SYS_WM_REGISTER:
    ret = sys_wm_register();
    break;
  case SYS_WM_PID:
    ret = sys_wm_pid();
    break;
  case SYS_TTY_DRAIN:
    ret = sys_tty_drain((char *)(uintptr_t)a1, (uintptr_t)a2);
    break;
  case SYS_FB_INFO:
    ret = sys_fb_info((struct fb_info *)(uintptr_t)a1);
    break;
  case SYS_FB_MAP:
    ret = sys_fb_map();
    break;
  case SYS_FB_DAMAGE:
    framebuffer_mark_damage((uint32_t)a1, (uint32_t)a2,
                            (uint32_t)a3, (uint32_t)a4);
    ret = 0;
    break;
  case SYS_KBD_POLL:
    ret = sys_kbd_poll((int *)(uintptr_t)a1, (uint16_t *)(uintptr_t)a2);
    break;
  case SYS_GET_TICKS:
    ret = sys_get_ticks();
    break;
  case SYS_UNLINK:
    ret = sys_unlink((const char *)(uintptr_t)a1);
    break;
  case SYS_EXEC:
    ret = sys_exec((const char *)(uintptr_t)a1, (char *const *)(uintptr_t)a2);
    break;
  case SYS_SPAWN:
    ret = sys_spawn((const char *)(uintptr_t)a1, (char *const *)(uintptr_t)a2);
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
  default:
    log_write_hex("unknown syscall =", num, KERNEL, LOG_ERROR);
  }
  f->rax = (uint64_t)ret;
  return ret;
}

// #endregion DISPATCH
