/* kernel/arch/syscall.c — SYSCALL entry + dispatcher.
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
 * action, and returns a long that the asm path writes back into the
 * frame's rax slot (and thus userspace's rax on SYSRET).
 *
 * Pointer validation is best-effort: anything taken from userspace is
 * range-checked against [0, USER_LOW_LIMIT) and walked via
 * vmm_translate_in to confirm it's actually mapped USER+writable. The
 * trade-off is "trusted enough to not crash the kernel"; a real OS would
 * do per-page copy-in/copy-out into kernel buffers.
 */
#include "arch/syscall.h"
#include "devices/io.h"
#include "devices/pit.h"
#include "devices/serial.h"
#include "display/framebuffer.h"
#include "display/tty.h"
#include "fs/fat.h"
#include "input/keyboard.h"
#include "input/mouse.h"
#include "loader/process.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "msg/msg.h"
#include "sched/sched.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stddef.h>
#include <stdint.h>


/* User VA boundaries (USER_FB_BASE, USER_MMAP_BASE, USER_MMAP_LIMIT,
 * USER_VA_MIN/MAX, stack) all come from loader/process.h. */
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

#define MAX_SHMEM_PAGES 4096 /* per-call cap; 16 MiB */

struct fb_info {
  uint64_t width;
  uint64_t height;
  uint64_t pitch;
  uint64_t bpp;
};

extern void syscall_entry(void);
extern uint64_t kernel_rsp_top;

static int resolve_path(const char *path, char *out, size_t max);

static int fd_alloc_for(struct task *t, struct fat_file *f) {
  if (!t || !f)
    return -1;
  for (int i = 3; i < TASK_MAX_FDS; i++) {
    if (!t->fds[i]) {
      t->fds[i] = f;
      return i;
    }
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
    if (!phys)
      return -1;
    if (vmm_map(USER_FB_BASE + (uint64_t)i * 4096, phys,
                VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
      return -1;
    }
  }
  return USER_FB_BASE;
}

static void sys_mmap_rollback(struct task *t, uint64_t base, uint64_t pages) {
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t va = base + i * 4096;
    uint64_t phys = vmm_translate_in(t->user_pml4, va);
    if (!phys)
      continue;
    vmm_unmap_in(t->user_pml4, va);
    pmm_free_frame(phys & ~4095ULL);
  }
}

/* Translate PROT_* into PTE bits. Returns 0 for a protection this VMM
 * cannot express, which callers must treat as an error — 0 is never a
 * legal user PTE flag set (VMM_USER is always required). */
static uint64_t prot_to_pte(int prot) {
  if (prot == PROT_NONE)
    return 0;                       /* see the PROT_NONE note in syscall.h */
  if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return 0;
  if (!(prot & PROT_READ))
    return 0;                       /* x86 has no write-only or exec-only  */

  uint64_t flags = VMM_PRESENT | VMM_USER;
  if (prot & PROT_WRITE)
    flags |= VMM_WRITE;
  if (!(prot & PROT_EXEC))
    flags |= VMM_NX;
  return flags;
}

/* True if `base .. base+bytes` is a legal user range that does not run
 * into the stack. Wrap-around is checked by the caller's overflow test. */
static int user_range_ok(uint64_t base, uint64_t bytes) {
  if (bytes == 0 || base < USER_VA_MIN)
    return 0;
  if (base + bytes < base || base + bytes > USER_VA_MAX)
    return 0;
  if (base < USER_STACK_TOP && base + bytes > USER_STACK_LOW)
    return 0;
  return 1;
}

/* Every page in the range must be free for a MAP_FIXED to succeed. */
static int range_is_unmapped(struct task *t, uint64_t base, uint64_t bytes) {
  for (uint64_t off = 0; off < bytes; off += 4096) {
    if (vmm_translate_in(t->user_pml4, base + off))
      return 0;
  }
  return 1;
}

/* Record a freed arena range for reuse, merging with any hole it touches
 * so repeated map/unmap of adjacent blocks doesn't shred the list. */
static void hole_add(struct task *t, uint64_t base, uint64_t bytes) {
  for (int i = 0; i < TASK_MMAP_HOLES; i++) {
    struct vm_hole *h = &t->mmap_holes[i];
    if (!h->len)
      continue;
    if (h->base + h->len == base) {
      h->len += bytes;
      return;
    }
    if (base + bytes == h->base) {
      h->base = base;
      h->len += bytes;
      return;
    }
  }

  for (int i = 0; i < TASK_MMAP_HOLES; i++) {
    if (!t->mmap_holes[i].len) {
      t->mmap_holes[i].base = base;
      t->mmap_holes[i].len = bytes;
      return;
    }
  }
  /* List full: the range stays unmapped but its VA is not reused. */
  log_write("mmap: hole list full, leaking user VA", KERNEL, LOG_WARN);
}

/* First-fit over the free list, then the bump pointer. Returns 0 when the
 * arena is exhausted — never a valid address, since the arena starts well
 * above USER_VA_MIN. */
static uint64_t arena_alloc(struct task *t, uint64_t bytes) {
  for (int i = 0; i < TASK_MMAP_HOLES; i++) {
    struct vm_hole *h = &t->mmap_holes[i];
    if (!h->len || h->len < bytes)
      continue;
    uint64_t base = h->base;
    h->base += bytes;
    h->len -= bytes;
    return base;
  }

  uint64_t base = t->mmap_next_va;
  if (base + bytes < base || base + bytes > USER_MMAP_LIMIT)
    return 0;
  t->mmap_next_va = base + bytes;
  return base;
}

/* mmap(addr, len, prot, flags).
 *
 * Anonymous, private, eagerly backed: every page gets a zeroed frame at
 * call time. There is no demand paging and no file-backed mapping — a
 * loader reads section bytes in with read() after mapping the range.
 *
 * Without MAP_FIXED, `addr` is ignored and the range comes from the arena.
 * With MAP_FIXED, `addr` must be page-aligned and entirely unmapped. */
static long sys_mmap(uint64_t addr, long len, int prot, int flags) {
  if (len <= 0)
    return -1;

  struct task *t = task_current();
  if (!t || !t->user_pml4)
    return -1;

  uint64_t pte_flags = prot_to_pte(prot);
  if (!pte_flags)
    return -1;

  uint64_t bytes = ((uint64_t)len + 4095) & ~4095ULL;
  if (bytes < (uint64_t)len)          /* rounding wrapped */
    return -1;

  uint64_t base;
  if (flags & MAP_FIXED) {
    if (addr & 4095)
      return -1;
    if (!user_range_ok(addr, bytes))
      return -1;
    if (!range_is_unmapped(t, addr, bytes))
      return -1;
    base = addr;
  } else {
    base = arena_alloc(t, bytes);
    if (!base)
      return -1;
  }

  uint64_t pages = bytes / 4096;
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) {
      sys_mmap_rollback(t, base, i);
      return -1;
    }

    memset(phys_to_virt(phys), 0, 4096);
    if (vmm_map_in(t->user_pml4, base + i * 4096, phys, pte_flags) != 0) {
      pmm_free_frame(phys);
      sys_mmap_rollback(t, base, i);
      return -1;
    }
  }

  return (long)base;
}

/* mprotect(addr, len, prot).
 *
 * All-or-nothing: the range is validated before a single PTE changes, so
 * a partial failure can't leave a PE image half RX and half RW. Pages must
 * already be mapped — this only changes permissions, it never allocates. */
static long sys_mprotect(uint64_t addr, long len, int prot) {
  if (len <= 0 || (addr & 4095))
    return -1;

  struct task *t = task_current();
  if (!t || !t->user_pml4)
    return -1;

  uint64_t pte_flags = prot_to_pte(prot);
  if (!pte_flags)
    return -1;

  uint64_t bytes = ((uint64_t)len + 4095) & ~4095ULL;
  if (bytes < (uint64_t)len || !user_range_ok(addr, bytes))
    return -1;

  for (uint64_t off = 0; off < bytes; off += 4096) {
    if (!vmm_translate_in(t->user_pml4, addr + off))
      return -1;
  }

  for (uint64_t off = 0; off < bytes; off += 4096) {
    if (vmm_protect_in(t->user_pml4, addr + off, pte_flags) != 0)
      return -1;
  }
  return 0;
}

/* munmap(addr, len).
 *
 * Unmaps whole pages and returns their frames to the PMM, except frames
 * carrying VMM_SHARED: those belong to whichever task shared them in, and
 * freeing one here would hand a live page back to the allocator. Ranges
 * inside the mmap arena go on the free list; MAP_FIXED ranges outside it
 * need no bookkeeping, since fixed mappings are placed by the caller and
 * collision-checked at map time.
 *
 * Unmapping a range with holes in it is not an error — POSIX says the same
 * — so this reports success as long as the range itself is legal. */
static long sys_munmap(uint64_t addr, long len) {
  if (len <= 0 || (addr & 4095))
    return -1;

  struct task *t = task_current();
  if (!t || !t->user_pml4)
    return -1;

  uint64_t bytes = ((uint64_t)len + 4095) & ~4095ULL;
  if (bytes < (uint64_t)len || !user_range_ok(addr, bytes))
    return -1;

  for (uint64_t off = 0; off < bytes; off += 4096) {
    uint64_t va = addr + off;
    uint64_t entry = vmm_entry_in(t->user_pml4, va);
    if (!entry)
      continue;
    vmm_unmap_in(t->user_pml4, va);
    /* Mask to bits 12-51: VMM_NX lives at bit 63, so stripping the low
     * flag bits alone would hand the PMM an address with it still set. */
    if (!(entry & VMM_SHARED))
      pmm_free_frame(entry & VMM_ADDR_MASK);
  }

  if (addr >= USER_MMAP_BASE && addr + bytes <= USER_MMAP_LIMIT)
    hole_add(t, addr, bytes);
  return 0;
}

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

static long sys_write(long fd, const void *buf, long n) {
  if (fd == 1 || fd == 2) {
    if (!buf || n < 0)
      return -1;

    tty_write((const char *)buf, (size_t)n);

    const char *p = (const char *)buf;
    for (long i = 0; i < n; i++)
      serial_write_char(p[i]);

    return n;
  }

  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !t->fds[fd])
    return -1;

  return (long)fat_write(t->fds[fd], buf, (size_t)n);
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

static long sys_mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons) {
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
  tty_write(buf, (size_t)n);
  return n;
}

static long sys_con_clear(void) {
  tty_clear();
  return 0;
}

static long sys_con_push(void) { return tty_push(); }

static long sys_con_pop(void) { return tty_pop(); }

static long sys_con_zoom(long delta) { return tty_zoom((int)delta); }

static long sys_sleep_ticks(long n) {
  if (n <= 0) {
    task_yield();
    return 0;
  }
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
static long sys_shmem_share(long target_pid, uint64_t in_va, long npages,
                            uint64_t *out_target_va) {
  if (!out_target_va || target_pid <= 0 || npages <= 0)
    return -1;
  if (npages > MAX_SHMEM_PAGES)
    return -1;
  if (in_va & 0xFFFULL)
    return -1;

  struct task *me = task_current();
  if (!me || !me->user_pml4)
    return -1;

  struct task *target = task_find((int)target_pid);
  if (!target || !target->user_pml4)
    return -1;
  if (target->shmem_next_va == 0)
    target->shmem_next_va = 0x0000000080000000ULL; /* defensive */

  uint64_t target_va = target->shmem_next_va;
  /* VMM_SHARED marks the PTE so the target's exit cleanup (free_user_pml4)
   * skips pmm_free_frame on these phys frames — the caller still owns them. */
  uint64_t flags = VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_SHARED;

  for (long i = 0; i < npages; i++) {
    uint64_t phys = vmm_translate_in(me->user_pml4, in_va + (uint64_t)i * 4096);
    if (!phys)
      return -1;
    if (vmm_map_in(target->user_pml4, target_va + (uint64_t)i * 4096, phys,
                   flags) != 0) {
      return -1;
    }
  }

  target->shmem_next_va = target_va + (uint64_t)npages * 4096;
  /* These frames are now co-mapped by a task that will not free them.
   * Recording that keeps the automatic reaper from releasing our address
   * space underneath the receiver. */
  me->shmem_shared_out += (int)npages;
  *out_target_va = target_va;
  return 0;
}

/* Unmap shared pages from the target process. `va` is an address in the
 * TARGET's address space — the value sys_shmem_share wrote back through
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
static long sys_shmem_unshare(long target_pid, uint64_t va, long npages) {
  if (target_pid <= 0 || npages <= 0 || npages > MAX_SHMEM_PAGES)
    return -1;
  if (va & 0xFFFULL)
    return -1;

  struct task *me = task_current();
  if (!me || !me->user_pml4)
    return -1;

  struct task *target = task_find((int)target_pid);
  if (!target || !target->user_pml4)
    return -1;

  /* Confine the range to the shmem arena, where sys_shmem_share places
   * every mapping it makes.
   *
   * This is not tidiness. process_pml4_create copies PML4[256..511] and
   * the low-1 GiB PDPT entry into every process by physical address, so
   * those page tables are the same memory in all of them. Unmapping an
   * address down there would clear a PTE the kernel and every other
   * process walk, not just this target's view of it. */
  uint64_t bytes = (uint64_t)npages * 4096;
  if (va < USER_SHMEM_BASE || !user_range_ok(va, bytes))
    return -1;

  /* Only pages carrying VMM_SHARED arrived through a share, so only those
   * are ours to take away. Without this check any task could hand another
   * task's pid to unshare and unmap its stack or its text.
   *
   * It still does not prove *this* task was the one that shared them —
   * that needs per-share ownership the kernel does not record yet — but
   * it keeps the blast radius inside genuinely shared pages. */
  long removed = 0;
  for (long i = 0; i < npages; i++) {
    uint64_t target_va = va + (uint64_t)i * 4096;
    uint64_t entry = vmm_entry_in(target->user_pml4, target_va);
    if (!entry || !(entry & VMM_SHARED))
      continue;
    vmm_unmap_in(target->user_pml4, target_va);
    removed++;
  }

  /* Decrement by what actually went away, and never past zero.
   * task_reap_unclaimed reads a positive count as "receivers still map my
   * frames" and declines to reap; a count driven negative would let this
   * task be reaped and free_user_pml4 hand those frames back to the PMM
   * while a receiver was still writing through them. */
  if (removed >= me->shmem_shared_out)
    me->shmem_shared_out = 0;
  else
    me->shmem_shared_out -= (int)removed;

  return 0;
}

static long sys_wm_register(void) {
  struct task *t = task_current();
  if (!t)
    return -1;
  int rc = msg_input_owner_register(t->pid);
  if (rc == 0)
    tty_set_active(0);
  return rc;
}

static long sys_wm_pid(void) { return msg_input_owner(); }

static long sys_tty_drain(char *out, long max) {
  if (!out || max <= 0)
    return 0;
  return (long)tty_drain(out, (size_t)max);
}

/* Block-wait `seconds` using PIT ticks (10ms each at 100Hz). */
static void delay_seconds(long seconds) {
  if (seconds <= 0)
    return;
  extern uint64_t pit_ticks(void);
  uint64_t target = pit_ticks() + (uint64_t)seconds * 100;
  while (pit_ticks() < target)
    __asm__ volatile("hlt");
}

/* Try several poweroff mechanisms; halt if all fail. */
static void hw_shutdown(void) {
  log_write("powering off...", KERNEL, LOG_INFO);
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
  /* drain 8042 input buffer */
  while (inb(0x64) & 0x02) {
    (void)inb(0x60);
  }
  outb(0x64, 0xFE);  /* keyboard controller pulse */
  outb(0xCF9, 0x06); /* PIIX/q35 reset */
  /* triple-fault: null IDT + INT3 */
  struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
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
    const size_t prefix = 8;
    memcpy(buf, "reason: ", prefix);
    size_t rl = strlen(reason);
    if (rl > sizeof(buf) - prefix - 1)
      rl = sizeof(buf) - prefix - 1;
    memcpy(buf + prefix, reason, rl);
    buf[prefix + rl] = 0;
    log_write(buf, KERNEL, LOG_INFO);
  }
  delay_seconds(time);
  hw_shutdown();
  return 0; /* unreachable */
}

static long sys_reboot(long time) {
  log_write_hex("reboot in (s) =", time, KERNEL, LOG_INFO);
  delay_seconds(time);
  hw_reboot();
  return 0; /* unreachable */
}

/* O_CREAT = 0x40, O_TRUNC = 0x200, O_WRONLY = 1 (match fcntl.h) */
#define O_CREAT 0x40
#define O_TRUNC 0x200

static int path_is_absolute(const char *path) { return path && path[0] == '/'; }

static int path_append_component(char *out, size_t *len, size_t max,
                                 const char *start, size_t n) {
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
  for (size_t i = 0; i < n; i++)
    out[(*len)++] = start[i];
  out[*len] = 0;
  return 0;
}

static int resolve_path(const char *path, char *out, size_t max) {
  if (!path || !out || max < 2)
    return -1;

  struct task *t = task_current();
  if (!t)
    return -1;

  size_t len = 0;
  out[0] = '/';
  out[1] = 0;
  len = 1;

  if (!path_is_absolute(path)) {
    len = 0;
    while (len < max - 1 && t->cwd[len]) {
      out[len] = t->cwd[len];
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

    if (path_append_component(out, &len, max, start, (size_t)(p - start)) != 0)
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

  struct fat_file *f = (struct fat_file *)kmalloc(sizeof(*f));
  if (!f)
    return -1;

  int rc = fat_open(resolved, f);
  if (rc != 0 && (flags & O_CREAT)) {
    rc = fat_create(resolved, f);
  } else if (rc == 0 && (flags & O_TRUNC)) {
    kfree(f);
    fat_unlink(resolved);
    f = (struct fat_file *)kmalloc(sizeof(*f));
    if (!f)
      return -1;
    rc = fat_create(resolved, f);
  }

  if (rc != 0) {
    kfree(f);
    return -1;
  }

  int fd = fd_alloc_for(t, f);
  if (fd < 0) {
    kfree(f);
    return -1;
  }

  return fd;
}

static long sys_unlink(const char *path) {
  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return fat_unlink(resolved);
}

static long sys_read(int fd, void *buf, size_t n) {
  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !t->fds[fd])
    return -1;
  return (long)fat_read(t->fds[fd], buf, n);
}

static void stat_from_fat(const struct fat_stat *fs, struct stat_user *out) {
  out->size = fs->size;
  out->first_cluster = fs->first_cluster;
  out->type = fs->is_dir ? STAT_TYPE_DIR : STAT_TYPE_FILE;
  out->attr = fs->attr;
}

static long sys_stat(const char *path, struct stat_user *out) {
  if (!path || !out)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  struct fat_stat fs;
  if (fat_stat(resolved, &fs) != 0)
    return -1;

  stat_from_fat(&fs, out);
  return 0;
}

/* fstat works off the open handle rather than re-resolving a path, so it
 * still reports the right size for a file that has been written through
 * this fd — and keeps working if the name is unlinked while open. */
static long sys_fstat(int fd, struct stat_user *out) {
  struct task *t = task_current();
  if (!t || !out || fd < 3 || fd >= TASK_MAX_FDS || !t->fds[fd])
    return -1;

  struct fat_file *f = t->fds[fd];
  out->size = f->size;
  out->first_cluster = f->first_cluster;
  out->type = STAT_TYPE_FILE;   /* fat_open refuses directories */
  out->attr = 0;
  return 0;
}

static long sys_lseek(int fd, long off, int whence) {
  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !t->fds[fd])
    return -1;
  struct fat_file *f = t->fds[fd];
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
  struct task *t = task_current();
  if (!t || fd < 3 || fd >= TASK_MAX_FDS || !t->fds[fd])
    return -1;
  kfree(t->fds[fd]);
  t->fds[fd] = 0;
  return 0;
}

static long sys_readdir(uint32_t *index, char *buf, size_t n) {
  struct task *t = task_current();
  if (!t)
    return -1;
  return fat_read_dir(t->cwd, index, buf, n);
}

static long sys_readdir_path(const char *path, uint32_t *index, char *buf,
                             size_t n) {
  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return fat_read_dir(resolved, index, buf, n);
}

static long sys_mkdir(const char *path) {
  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;
  return fat_mkdir(resolved);
}

static long sys_chdir(const char *path) {
  struct task *t = task_current();
  if (!t || !path)
    return -1;

  char resolved[TASK_CWD_MAX];
  if (resolve_path(path, resolved, sizeof(resolved)) != 0)
    return -1;

  /* Existence check by stat rather than by reading an entry: a directory
   * whose first name is long would not fit in a probe buffer, and an
   * empty directory has no entry to read at all. */
  struct fat_stat st;
  if (fat_stat(resolved, &st) != 0 || !st.is_dir)
    return -1;

  size_t i = 0;
  while (i < TASK_CWD_MAX - 1 && resolved[i]) {
    t->cwd[i] = resolved[i];
    i++;
  }
  t->cwd[i] = 0;
  return 0;
}

static long sys_getcwd(char *buf, size_t size) {
  struct task *t = task_current();
  if (!t || !buf || size == 0)
    return -1;

  size_t i = 0;
  while (i + 1 < size && t->cwd[i]) {
    buf[i] = t->cwd[i];
    i++;
  }
  buf[i] = 0;

  if (t->cwd[i])
    return -1;

  return 0;
}


/* Userspace-visible proc_info layout. MUST match struct proc_info in
 * userspace/lib/syscall.h byte-for-byte. */
struct proc_info_user {
  uint64_t ticks_run;
  int pid;
  int parent_pid;
  int state;
  char name[16];
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
    for (size_t k = 0; k < sizeof(out[i].name); k++) {
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

static long sys_thread_create(uint64_t entry, uint64_t user_stack, uint64_t u_arg) {
    struct task *t = task_spawn_thread(entry, user_stack);
    if (!t) return -1;
    t->user_arg = u_arg;
    return t->pid;
}

static long sys_thread_exit(void) {
    struct task *t = task_current();
    if (!t) return -1;

    /* Threads share one PML4, so the last one out frees it. The decrement
     * must be atomic: two threads exiting concurrently would otherwise both
     * read the same count and either double-free or leak the address space. */
    int new_count = __atomic_sub_fetch(t->pml4_ref_count, 1, __ATOMIC_ACQ_REL);

    if (new_count <= 0) {
        kfree(t->pml4_ref_count);
        t->pml4_ref_count = 0;
        task_exit(0);            /* tears down the address space too */
    } else {
        /* Detach from the shared PML4 first — task_exit_thread must not
         * reap an address space its siblings are still running on. */
        t->user_pml4 = 0;
        t->pml4_ref_count = 0;
        task_exit_thread();
    }

    return 0;
}

static long sys_thread_join(long tid) {
    struct task *target = task_find((int)tid);
    if (!target) return -1;

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
static long sys_futex_wait(uint32_t *addr, uint32_t expected) {
    struct task *t = task_current();
    if (!t || !t->user_pml4) return -1;
    if (!addr) return -1;

    if ((uint64_t)addr >= USER_VA_MAX) return -1;

    /* Futexes key on the physical frame, so two tasks with the same page
     * mapped at different VAs still queue on the same futex. */
    uint64_t phys = vmm_translate_in(t->user_pml4, (uint64_t)addr);
    if (!phys) return -1;
    phys &= VMM_ADDR_MASK;

    if (*addr != expected) {
        return -1;
    }

    t->futex_addr = phys;
    task_block(0); /* 0 = indefinite; only a futex wake releases us */
    return 0;
}

/* Wake exactly one waiter. Callers needing wake-all must loop. */
static long sys_futex_wake(uint32_t *addr) {
    struct task *me = task_current();
    if (!me || !me->user_pml4) return -1;
    if (!addr) return -1;

    uint64_t phys = vmm_translate_in(me->user_pml4, (uint64_t)addr);
    if (!phys) return -1;
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
    ret = sys_mmap((uint64_t)a1, a2, (int)a3, (int)a4);
    break;
  case SYS_MPROTECT:
    ret = sys_mprotect((uint64_t)a1, a2, (int)a3);
    break;
  case SYS_MUNMAP:
    ret = sys_munmap((uint64_t)a1, a2);
    break;
  case SYS_STAT:
    ret = sys_stat((const char *)(uintptr_t)a1,
                   (struct stat_user *)(uintptr_t)a2);
    break;
  case SYS_FSTAT:
    ret = sys_fstat((int)a1, (struct stat_user *)(uintptr_t)a2);
    break;
  case SYS_READDIR:
    ret = sys_readdir((uint32_t *)(uintptr_t)a1, (char *)(uintptr_t)a2, a3);
    break;
  case SYS_READDIR_PATH:
    ret =
        sys_readdir_path((const char *)(uintptr_t)a1, (uint32_t *)(uintptr_t)a2,
                         (char *)(uintptr_t)a3, (size_t)a4);
    break;
  case SYS_CHDIR:
    ret = sys_chdir((const char *)(uintptr_t)a1);
    break;

  case SYS_GETCWD:
    ret = sys_getcwd((char *)(uintptr_t)a1, (size_t)a2);
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
    ret = sys_mouse_pos((int32_t *)(uintptr_t)a1, (int32_t *)(uintptr_t)a2,
                        (uint8_t *)(uintptr_t)a3);
    break;
  case SYS_CON_WRITE:
    ret = sys_con_write((const char *)(uintptr_t)a1, (uintptr_t)a2);
    break;
  case SYS_CON_ZOOM:
    ret = sys_con_zoom(a1);
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
    ret = sys_shmem_share((uintptr_t)a1, (uint64_t)(uintptr_t)a2, (uintptr_t)a3,
                          (uint64_t *)(uintptr_t)a4);
    break;
  case SYS_SHMEM_UNSHARE:
    ret = sys_shmem_unshare((uintptr_t)a1, (uint64_t)(uintptr_t)a2, (uintptr_t)a3);
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
    framebuffer_mark_damage((uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                            (uint32_t)a4);
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
    ret = sys_thread_join((uintptr_t)a1);
    break;
  case SYS_FUTEX_WAIT:
    ret = sys_futex_wait((uint32_t *)(uintptr_t)a1, (uint32_t)a2);
    break;
  case SYS_FUTEX_WAKE:
    ret = sys_futex_wake((uint32_t *)(uintptr_t)a1);
    break;
  default:
    log_write_hex("unknown syscall =", num, KERNEL, LOG_ERROR);
  }
  f->rax = (uint64_t)ret;
  return ret;
}
