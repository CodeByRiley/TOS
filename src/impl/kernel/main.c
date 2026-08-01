/* src/impl/kernel/main.c — kernel entry point.
 *
 * kernel_main is the C entry called from the assembly boot trampoline
 * (src/impl/x86_64/boot/main64.asm) once long mode is up. Brings the
 * kernel up in dependency order:
 *
 *   1. serial + VGA print  — earliest visible output
 *   2. GDT + percpu (BSP)  — segments + gs-relative state
 *   3. PIC remap + IDT     — interrupt routing
 *   4. PIT                 — tick source for sched + sleeps
 *   5. PMM + VMM + heap    — memory subsystems
 *   6. ACPI + LAPIC + SMP  — multi-CPU bring-up
 *   7. PCI scan            — device enumeration
 *   8. virtio-gpu          — optional; falls back to MB2 fb on failure
 *   9. FAT (rootfs)        — file access for ELF load
 *  10. keyboard / mouse    — input
 *  11. TTY                 — framebuffer-backed kernel console
 *  12. msg + syscall       — userspace ABI
 *  13. sched_init + spawn  — launches winman + the userspace shell
 *
 * After init, the BSP becomes the scheduler's idle task and lets ring 3
 * take over.
 */
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
#include "drivers/driver.h"
#include "drivers/video/nvidia.h"
#include "fs/fat.h"
#include "input/keyboard.h"
#include "input/mouse.h"
#include "interrupts/idt.h"
#include "interrupts/pic.h"
#include "loader/elf.h"
#include "loader/process.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
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

  /* Register hardware drivers, scan PCI, and bind discovered devices.
   * NVIDIA currently keeps the firmware framebuffer active while recording
   * the GPU's PCI resources for later native modesetting work. */
  driver_core_init();
  if (nvidia_driver_register() != 0) {
    log_write("nvidia: driver registration failed", KERNEL, LOG_ERROR);
  }
  pci_init();
  driver_probe_pci_devices();

  /* Pick who drives scanout. The test is whether a native driver has
   * actually taken the display, not whether its hardware merely exists:
   * an NVIDIA GPU that is only detected still leaves the firmware
   * framebuffer in charge, and in that case virtio-gpu is strictly better
   * than staying in MB2 fixed mode. Probing for virtio costs nothing when
   * the device is absent.
   *
   * Once NVIDIA modesetting lands this needs revisiting: nvidia_driver_late_init
   * runs after the root filesystem mounts, which is well past this point, so a
   * device that can drive scanout cannot say so yet. */
  if (!nvidia_display_active()) {
    if (nvidia_device_count() > 0) {
      log_write("display: NVIDIA detected but not driving scanout",
                KERNEL, LOG_INFO);
    }
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
  } else {
    log_write("display: NVIDIA driving scanout, keeping its framebuffer",
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
    if (fat_init(phys_to_virt(m->mod_start),
                 m->mod_end - m->mod_start) != 0) {
      log_write("rootfs: FAT initialisation failed", FILESYS, LOG_ERROR);
    } else {
      log_write("rootfs: fat module initialised", FILESYS, LOG_INFO);
      nvidia_driver_late_init();
    }

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
