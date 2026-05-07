#include "arch/syscall.h"
#include "devices/io.h"
#include "devices/pit.h"
#include "devices/serial.h"
#include "display/framebuffer.h"
#include "fs/fat.h"
#include "input/keyboard.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

#define USER_FB_BASE 0x0000000060000000ULL

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MAX_FDS 32

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
  /* Re-map every call: process_exec wipes user PDPT between programs,
     so the static "mapped" cache would lie. vmm_map overwrites cleanly. */
  uint64_t phys  = framebuffer_phys();
  uint64_t bytes = (uint64_t)framebuffer_pitch() * framebuffer_height();
  uint64_t pages = (bytes + 4095) / 4096;

  for (uint64_t i = 0; i < pages; i++) {
    if (vmm_map(USER_FB_BASE + i * 4096, phys + i * 4096,
                VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
      return -1;
    }
  }
  return USER_FB_BASE;
}

static long sys_kbd_poll(int *pressed, uint16_t *key) {
  if (!pressed || !key)
    return -1;
  return keyboard_poll(pressed, key);
}

static long sys_get_ticks(void) {
  return (long)(pit_ticks() * 10); // 100Hz → ms
}
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

  // log_write_hex("EFER  =", rdmsr(MSR_EFER), KERNEL, LOG_INFO);
  // log_write_hex("STAR  =", rdmsr(MSR_STAR), KERNEL, LOG_INFO);
  // log_write_hex("LSTAR =", rdmsr(MSR_LSTAR), KERNEL, LOG_INFO);
  // log_write_hex("FMASK =", rdmsr(MSR_FMASK), KERNEL, LOG_INFO);
}

// individual syscall handlers
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

extern void user_exit_jump(long code) __attribute__((noreturn));

/* legacy stubs kept so older callers link; no longer used */
char  pending_exec_path[64] = {0};
int   pending_exec_set      = 0;

extern long process_exec(const char *path, char *const argv[]);

static long sys_exec(const char *path, char *const argv[]) {
    if (!path) return -1;
    return process_exec(path, argv);
}

static long sys_exit(long code) {
  log_write_hex("user exit code =", code, KERNEL, LOG_INFO);
  user_exit_jump(code);
  return 0; // unreachable
}

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

long syscall_dispatch(struct syscall_frame *f) {
  long num = (long)f->rax;
  long a1 = (long)f->rdi;
  long a2 = (long)f->rsi;
  long a3 = (long)f->rdx;
  // args 4-6 use r10/r8/r9 in syscall ABI
  long a4 = (long)f->r10;
  long a5 = (long)f->r8;
  long a6 = (long)f->r9;
  (void)a4;
  (void)a5;
  (void)a6;

  long ret = -1;
  switch (num) {
  case SYS_WRITE:
    ret = sys_write(a1, (const void *)a2, a3);
    break;
  case SYS_READ:
    ret = sys_read(a1, (void *)a2, a3);
    break;
  case SYS_OPEN:
    ret = sys_open((const char *)a1, a2);
    break;
  case SYS_CLOSE:
    ret = sys_close(a1);
    break;
  case SYS_LSEEK:
    ret = sys_lseek(a1, a2, a3);
    break;
  case SYS_READDIR:
    ret = sys_readdir((uint32_t *)a1, (char *)a2, a3);
    break;
  case SYS_EXIT:
    ret = sys_exit(a1);
    break;
  case SYS_FB_INFO:
    ret = sys_fb_info((struct fb_info *)a1);
    break;
  case SYS_FB_MAP:
    ret = sys_fb_map();
    break;
  case SYS_KBD_POLL:
    ret = sys_kbd_poll((int *)a1, (uint16_t *)a2);
    break;
  case SYS_GET_TICKS:
    ret = sys_get_ticks();
    break;
  case SYS_UNLINK:
    ret = sys_unlink((const char *)a1);
    break;
  case SYS_EXEC:
    ret = sys_exec((const char *)a1, (char *const *)a2);
    break;
  case SYS_SHUTDOWN:
    ret = sys_shutdown(a1, (const char *)a2);
    break;
  case SYS_REBOOT:
    ret = sys_reboot(a1);
    break;
  default:
    log_write_hex("unknown syscall =", num, KERNEL, LOG_ERROR);
  }
  f->rax = (uint64_t)ret;
  return ret;
}
