/* Kernel entry point and staged subsystem initialization. */
#include "net/ksocket.h"
#include <acpi/acpi.h>
#include <arch/gdt.h>
#include <arch/percpu.h>
#include <arch/syscall.h>
#include <boot/multiboot2.h>
#include <boot/uefi.h>
#include <devices/lapic.h>
#include <devices/pit.h>
#include <devices/serial.h>
#include <devices/usb.h>
#include <display/fonts/ttf.h>
#include <display/framebuffer.h>
#include <display/print.h>
#include <display/tty.h>
#include <drivers/driver.h>
#include <drivers/network/eth/e1000/e1000.h>
#include <drivers/sound/sb16.h>
#include <drivers/storage/ahci.h>
#include <drivers/video/nvidia/nvidia.h>
#include <fs/rootfs.h>
#include <input/keyboard.h>
#include <input/mouse.h>
#include <interrupts/idt.h>
#include <interrupts/pic.h>
#include <isa/isa.h>
#include <loader/process.h>
#include <memory/memory.h>
#include <msg/msg.h>
#include <pci/pci.h>
#include <sched/sched.h>
#include <sched/smp.h>
#include <stdint.h>
#include <utilities/log.h>

extern u64 *kernel_pml4;
static u8 bsp_syscall_kstack[16384] ALIGNED(16);

static void early_console_init(void) {
  print_clear();
  print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
  serial_init();
  log_write("serial initialised", KERNEL, LOG_INFO);
}

static void arch_init(void) {
  log_write("initialising gdt", KERNEL, LOG_INFO);
  gdt_init();
  idt_init();
  log_write("idt initialised", KERNEL, LOG_INFO);
  pic_remap();
  pit_init(1000);
  log_write("pit initialised", KERNEL, LOG_INFO);
}

static void memory_init(u64 mb2_addr) {
  pmm_init(mb2_addr);
  vmm_init();
  vma_init();
  heap_init();
  log_write("pmm, vmm, heap initialised", KERNEL, LOG_INFO);
}

static void acpi_and_smp_init(u64 mb2_addr) {
  u8 bsp_lapic_id = 0;
  int cpu_count = 1;

  if (acpi_init(mb2_addr) == 0) {
    lapic_init(acpi_lapic_phys());
    bsp_lapic_id = (u8)lapic_id();
    cpu_count = acpi_cpu_count();
  } else {
    log_write("ACPI: SMP unavailable, staying UP", KERNEL, LOG_INFO);
  }

  /* GS-relative syscall entry is required even on the UP/no-ACPI path. */
  percpu_init_bsp(bsp_lapic_id);
  percpu_set_count(cpu_count);
  gdt_load_tss_this_cpu(0);
}

static void ipc_and_sched_init(void) {
  msg_init();
  log_write("message queue initialised", KERNEL, LOG_INFO);
  syscall_init_this_cpu((u64)(bsp_syscall_kstack + sizeof(bsp_syscall_kstack)));
  sched_init();
  log_write("scheduler initialised", KERNEL, LOG_INFO);
}

/* Which backend ends up driving the scanout is the display module's business;
 * whether that backend earns a scheduler thread is ours. */
static void display_start(u64 mb2_addr) {
  log_write("initialising display", KERNEL, LOG_INFO);
  display_init(mb2_addr);
  log_write("display initialised", KERNEL, LOG_INFO);

  if (framebuffer_needs_flush()) {
    struct task *flush = task_spawn(framebuffer_flush_thread_entry);
    if (flush) {
      sched_set_priority(flush, SCHED_PRIO_HIGH);
      log_write("display: fb flush thread spawned", KERNEL, LOG_INFO);
    } else {
      log_write("display: could not spawn fb flush thread", KERNEL, LOG_ERROR);
    }
  }
}

static void input_init(void) {
  keyboard_init();
  log_write("keyboard initialised", KERNEL, LOG_INFO);
  mouse_init();
  log_write("mouse initialised", KERNEL, LOG_INFO);
  mouse_set_bounds((int32_t)framebuffer_width(), (int32_t)framebuffer_height());
}

static void devices_init(void) {
  driver_core_init();
  if (nvidia_driver_register() != 0) {
    log_write("nvidia: driver registration failed", KERNEL, LOG_ERROR);
  }
  pci_init();
  driver_probe_pci_devices();
  isa_probe_devices();
  sb16_driver_init();
  e1000_driver_init();
  usb_init();
  ahci_init();
}

static void filesystem_init(u64 mb2_addr) {
  if (rootfs_mount(mb2_addr) != 0) {
    log_write("PANIC: Unable to mount any root filesystem!", KERNEL, LOG_ERROR);
    for (;;)
      __asm__ volatile("hlt");
  }
}

static void late_init(void) {
  nvidia_driver_late_init();
  ttf_init_font();
  if (g_sys_font != NULL) {
    tty_resize();
    static const char hello[] = "Hello from TTF!\n";
    tty_write(hello, sizeof(hello) - 1);
  }

  tty_init();
  log_write("tty: fallback text console ready", KERNEL, LOG_INFO);
  task_spawn(tty_thread_entry);
  log_write("tty: render thread spawned", KERNEL, LOG_INFO);

  task_spawn(udp_echo_thread);
  log_write("udp: echo server thread spawned", KERNEL, LOG_INFO);

  task_spawn(task_reaper_thread_entry);
  log_write("sched: zombie reaper thread spawned", KERNEL, LOG_INFO);
}

/* Ticks to wait for winman to register before concluding it is not coming.
 * Only the no-WM path depends on this: overshooting costs a few seconds of
 * blank kernel console, undershooting puts a second shell on the kernel
 * channel next to the one winman just opened. */
#define WM_REGISTER_GRACE_TICKS 300

/* Boot the userspace world, then supervise it.
 *
 * This task used to *be* the shell's parent in the blocking sense: it called
 * process_exec in a loop and restarted the shell every time it exited. That
 * tied the kernel's liveness to a userspace shell , closing the console
 * without a respawn returned from here, fell off the end of kernel_main, and
 * took the machine with it , and it capped the system at one shell, because
 * this task was the only thing running one.
 *
 * Shells now belong to winman. It opens a TTY channel per console window and
 * starts a shell on it, so it knows every console shell's pid and can close
 * one on request. What is left here is the fallback: if no window manager
 * registers, nothing else can give the user a prompt, so run one on the
 * kernel-rendered channel and wait for it. While a WM is up this loop does
 * nothing at all, and every shell in the system , including that fallback ,
 * can exit without consequence. */
static void init_task_entry(void) {
  char *winman_argv[] = {(char *)"winman", NULL};
  long winman_pid = process_spawn_async("/system/bin/winman.elf", winman_argv);
  if (winman_pid < 0)
    log_write("winman: launch failed , TTY-only mode", USER, LOG_INFO);
  else
    log_write_hex("winman: spawn returned pid =", (u64)winman_pid, USER,
                  LOG_INFO);

  for (int i = 0; i < WM_REGISTER_GRACE_TICKS && msg_input_owner() == 0; i++)
    task_sleep_ticks(1);

  for (;;) {
    if (msg_input_owner() == 0) {
      char *sh_argv[] = {(char *)"sh", NULL};
      long code = process_exec("/system/bin/sh.elf", sh_argv);
      log_write_hex("fallback shell exited code =", (u64)code, USER, LOG_INFO);
    }
    task_sleep_ticks(50);
  }
}

static void enable_interrupts_and_smp(void) {
  pic_clear_mask(0);
  pic_clear_mask(1);
  pic_clear_mask(2);  /* cascade */
  pic_clear_mask(12); /* mouse */
  __asm__ volatile("sti");

  smp_boot_aps();
  log_write("kernel booted", KERNEL, LOG_INFO);
}

void kernel_main(u64 mb2_addr) {
  early_console_init();
  uefi_init(mb2_addr);
  arch_init();
  memory_init(mb2_addr);
  acpi_and_smp_init(mb2_addr);
  ipc_and_sched_init();
  devices_init(); /* PCI + drivers first so display can see them */
  display_start(mb2_addr);
  input_init(); /* after real framebuffer exists */
  filesystem_init(mb2_addr);
  late_init();
  enable_interrupts_and_smp();
  init_task_entry();

  /* BSP becomes idle */
  for (;;)
    __asm__ volatile("hlt");
}
