#include "acpi/acpi.h"
#include "arch/gdt.h"
#include "arch/percpu.h"
#include "arch/syscall.h"
#include "boot/multiboot2.h"
#include "devices/lapic.h"
#include "devices/pit.h"
#include "devices/serial.h"
#include "display/framebuffer.h"
#include "display/print.h"
#include "display/tty.h"
#include "fs/fat.h"
#include "input/keyboard.h"
#include "input/mouse.h"
#include "interrupts/idt.h"
#include "interrupts/pic.h"
#include "loader/elf.h"
#include "loader/process.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "msg/msg.h"
#include "pci/pci.h"
#include "sched/sched.h"
#include "sched/smp.h"
#include "utilities/log.h"
#include "utilities/printf.h"
#include "virtio/virtio_gpu.h"
#include <stdint.h>

void kernel_main(uint64_t mb2_addr) {
  print_clear();
  print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
  serial_init();
  log_write("serial initialised", KERNEL, LOG_INFO);

  log_write("initialising gdt", KERNEL, LOG_INFO);
  gdt_init();

  log_write("initialising idt", KERNEL, LOG_INFO);
  idt_init();
  log_write("idt initialised", KERNEL, LOG_INFO);
  pic_remap();
  log_write("pic remapped", KERNEL, LOG_INFO);
  pit_init(100);
  log_write("pit initialised", KERNEL, LOG_INFO);
  keyboard_init();
  log_write("keyboard initialised", KERNEL, LOG_INFO);
  mouse_init();
  log_write("mouse initialised", KERNEL, LOG_INFO);

  log_write("initialising pmm, vmm, heap", KERNEL, LOG_INFO);
  pmm_init(mb2_addr);
  vmm_init();
  heap_init();
  log_write("pmm, vmm, heap initialised", KERNEL, LOG_INFO);


  /* ACPI -> MADT -> LAPIC base + CPU enumeration. SMP-only; safe to no-op
   * if firmware doesn't expose ACPI (we'd just stay UP). */
  if (acpi_init(mb2_addr) == 0) {
    lapic_init(acpi_lapic_phys());
    percpu_init_bsp((uint8_t)lapic_id());
    percpu_set_count(acpi_cpu_count());
    /* Hook BSP's per-CPU TSS pointer now that percpu is initialised. */
    gdt_load_tss_this_cpu(0);
  } else {
    log_write("ACPI: SMP unavailable, staying UP", KERNEL, LOG_INFO);
  }

  msg_init();
  log_write("message queue initialised", KERNEL, LOG_INFO);

  sched_init();
  log_write("scheduler initialised", KERNEL, LOG_INFO);

  log_write("initialising framebuffer", KERNEL, LOG_INFO);
  framebuffer_init(mb2_addr);
  log_write("framebuffer initialised", KERNEL, LOG_INFO);

  /* Bring up PCI then virtio-gpu. If the device is present we hand the
   * framebuffer over to it so the host window can drive resolution; if
   * absent or any step fails, framebuffer stays in MB2 (fixed-mode)
   * fallback and the rest of boot proceeds unchanged. */
  pci_init();
  if (virtio_gpu_init() == 0) {
    if (framebuffer_attach_virtio() != 0) {
      log_write("display: virtio attach failed, staying on MB2 fb",
                KERNEL, LOG_WARN);
    } else {
      task_spawn(framebuffer_flush_thread_entry);
      log_write("display: fb flush thread spawned", KERNEL, LOG_INFO);
    }
  } else {
    log_write("display: no virtio-gpu, staying on MB2 fb",
              KERNEL, LOG_INFO);
  }

  mouse_set_bounds((int32_t)framebuffer_width(), (int32_t)framebuffer_height());

  tty_init();
  log_write("tty: fallback text console ready", KERNEL, LOG_INFO);

  task_spawn(tty_thread_entry);
  log_write("tty: render thread spawned", KERNEL, LOG_INFO);

  // syscalls need a kernel stack for ring transitions
  static uint8_t syscall_kstack[16384] __attribute__((aligned(16)));
  syscall_init((uint64_t)(syscall_kstack + sizeof(syscall_kstack)));

  struct MB2_TAG_MODULE *m = mb2_find_module(mb2_addr, "rootfs");
  if (!m) {
    log_write("rootfs: no rootfs module", KERNEL, LOG_ERROR);
  } else {
    log_write_hex("rootfs: module phys =", (uint64_t)m->mod_start, FILESYS,
                  LOG_INFO);
    log_write_hex("rootfs: module size =", m->mod_end - m->mod_start, FILESYS,
                  LOG_INFO);
    log_write("rootfs: module found", FILESYS, LOG_INFO);
    fat_init((uint8_t *)(uint64_t)m->mod_start, m->mod_end - m->mod_start);
    log_write("rootfs: fat module initialised", FILESYS, LOG_INFO);

    pic_clear_mask(0);
    pic_clear_mask(1);
    pic_clear_mask(2);   /* slave PIC cascade — required for IRQ8..15 */
    pic_clear_mask(12);  /* PS/2 mouse */

    log_write("enabling interrupts", KERNEL, LOG_INFO);
    __asm__ volatile("sti");
    log_write("interrupts enabled", KERNEL, LOG_INFO);

    /* Bring up Application Processors. Needs PIT IRQs running (smp_delay_us
     * counts ticks) so it has to happen after sti. APs land in ap_main and
     * halt — they'll join the scheduler in a later stage. */
    smp_boot_aps();

    log_write("kernel booted", KERNEL, LOG_INFO);

    /* Launch the userspace window manager in the background — like DOS
     * starting WIN.COM, but here it runs as a sibling task alongside
     * the shell rather than replacing it. If WINMAN.ELF is missing or
     * fails to load we just don't have windows; the shell still boots
     * on the kernel TTY. */
    char *winman_argv[] = {(char *)"winman", 0};
    long winman_pid = process_spawn_async("WINMAN.ELF", winman_argv);
    if (winman_pid < 0) {
      log_write("winman: launch failed — TTY-only mode",
                USER, LOG_INFO);
    } else {
      log_write_hex("winman spawned pid =", winman_pid, USER, LOG_INFO);
    }

    char *sh_argv[] = {(char *)"sh", 0};
    while (1) {
      long code = process_exec("SH.ELF", sh_argv);
      log_write_hex("shell exited code =", code, USER, LOG_INFO);
    }
  }

  for (;;)
    __asm__ volatile("hlt");
}
