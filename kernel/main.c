/* kernel/main.c — kernel entry point.
 *
 * kernel_main is the C entry called from the assembly boot trampoline
 * (kernel/arch/x86_64/boot/main64.asm) once long mode is up. Brings the
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
#include <acpi/acpi.h>
#include <arch/gdt.h>
#include <arch/percpu.h>
#include <arch/syscall.h>
#include <boot/multiboot2.h>
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
#include <drivers/video/virtio/virtio_gpu.h>
#include <fs/fat.h>
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

extern struct AHCI_DEVICE_DATA *g_ahci_dev;
extern uint64_t *kernel_pml4;

void kernel_main(uint64_t mb2_addr) {
  print_clear();
  print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
  serial_init();
  log_write("serial initialised", KERNEL, LOG_INFO);

  log_write("initialising gdt", KERNEL, LOG_INFO);
  gdt_init();

  idt_init();
  log_write("idt initialised", KERNEL, LOG_INFO);
  pic_remap();
  pit_init(500);
  log_write("pit initialised", KERNEL, LOG_INFO);
  keyboard_init();
  log_write("keyboard initialised", KERNEL, LOG_INFO);
  mouse_init();
  log_write("mouse initialised", KERNEL, LOG_INFO);

  pmm_init(mb2_addr);
  vmm_init();
  vma_init();
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

  isa_probe_devices();
  sb16_driver_init();
  e1000_driver_init();
  usb_init();
  ahci_init();

  // log_write("Testing Demand Paging...", KERNEL, LOG_INFO);

  //  Allocate a 4KB virtual page using vmalloc
  //    (vmalloc allocates a VMA range, but we won't map it yet to test the
  //    fault)
  // Note: If you actually use vmalloc, it maps it. To test pure demand paging,
  // we can just unmap it immediately to simulate an unmapped VMA.
  // uint64_t test_page = vmalloc(4096);
  // vmm_unmap_in(kernel_pml4, test_page);

  // log_write("Unmapped test page. Preparing to write to it...", KERNEL,
  //           LOG_INFO);

  // // Write to it! This will trigger a page fault.
  // uint32_t *ptr = (uint32_t *)test_page;
  // *ptr = 0xDEADBEEF;

  // // Read it back to prove the handler mapped it properly
  // if (*ptr == 0xDEADBEEF) {
  //   log_write("Demand Paging Success! CPU dynamically mapped the page.", KERNEL,
  //             LOG_INFO);
  // } else {
  //   log_write("Demand Paging FAILED!", KERNEL, LOG_ERROR);
  // }

  /* Pick who drives scanout. The test is whether a native driver has
   * actually taken the display, not whether its hardware merely exists:
   * an NVIDIA GPU that is only detected still leaves the firmware
   * framebuffer in charge, and in that case virtio-gpu is strictly better
   * than staying in MB2 fixed mode. Probing for virtio costs nothing when
   * the device is absent.
   *
   * Once NVIDIA modesetting lands this needs revisiting:
   * nvidia_driver_late_init runs after the root filesystem mounts, which is
   * well past this point, so a device that can drive scanout cannot say so yet.
   */
  if (!nvidia_display_active()) {
    if (nvidia_device_count() > 0) {
      log_write("display: NVIDIA detected but not driving scanout", KERNEL,
                LOG_INFO);
    }
    if (virtio_gpu_init() == 0) {
      if (framebuffer_attach_virtio() != 0) {
        log_write("display: virtio attach failed, staying on MB2 fb", KERNEL,
                  LOG_WARN);
      } else {
        /* Everything anyone draws reaches the scanout through this thread,
         * so it must not queue behind whichever client happens to be busy. */
        struct task *flush = task_spawn(framebuffer_flush_thread_entry);
        if (flush)
          sched_set_priority(flush, SCHED_PRIO_HIGH);
        log_write("display: fb flush thread spawned", KERNEL, LOG_INFO);
      }
    } else {
      log_write("display: no virtio-gpu, staying on MB2 fb", KERNEL, LOG_INFO);
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
    for (;;)
      __asm__ volatile("hlt");
  }
  log_write("rootfs: module found", FILESYS, LOG_INFO);
  if (fat_mount_from_ahci(g_ahci_dev, 0) != 0) {
    log_write("Failed to mount rootfs!", KERNEL, LOG_ERROR);
  }
  if (fat_init(phys_to_virt(m->mod_start), m->mod_end - m->mod_start) != 0) {
    log_write("rootfs: FAT initialisation failed", FILESYS, LOG_ERROR);
  } else {
    nvidia_driver_late_init();
    ttf_init_font();
    if (g_sys_font != NULL) {
      tty_resize();
      static const char hello[] = "Hello from TTF!\n";
      tty_write(hello, sizeof(hello) - 1);
    }
  }

  pic_clear_mask(0);
  pic_clear_mask(1);
  pic_clear_mask(2);  /* slave PIC cascade — required for IRQ8..15 */
  pic_clear_mask(12); /* PS/2 mouse */

  __asm__ volatile("sti");

  /* Bring up Application Processors after PIT timing is available. APs run
   * the SMP-safe kernel work queue; userspace scheduling remains on the BSP. */
  smp_boot_aps();

  log_write("kernel booted", KERNEL, LOG_INFO);

  /* Launch the userspace window manager in the background — like DOS
   * starting WIN.COM, but here it runs as a sibling task alongside
   * the shell rather than replacing it. If /usr/bin/winman.elf is missing or
   * fails to load we just don't have windows; the shell still boots
   * on the kernel TTY. */
  char *winman_argv[] = {(char *)"winman", 0};
  long winman_pid = process_spawn_async("/usr/bin/winman.elf", winman_argv);
  if (winman_pid < 0) {
    log_write("winman: launch failed — TTY-only mode", USER, LOG_INFO);
  } else {
    log_write_hex("winman: spawn returned pid =", (uint64_t)winman_pid, USER,
                  LOG_INFO);
  }

  char *sh_argv[] = {(char *)"sh", 0};
  // while (1) {
  long code = process_exec("/bin/sh.elf", sh_argv);
  log_write_hex("shell exited code =", code, USER, LOG_INFO);
  //}

  for (;;)
    __asm__ volatile("hlt");
}
