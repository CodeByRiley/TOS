#include "arch/gdt.h"
#include "arch/syscall.h"
#include "boot/multiboot2.h"
#include "devices/pit.h"
#include "devices/serial.h"
#include "display/framebuffer.h"
#include "display/print.h"
#include "fs/fat.h"
#include "input/keyboard.h"
#include "interrupts/idt.h"
#include "interrupts/pic.h"
#include "loader/elf.h"
#include "loader/process.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include "utilities/printf.h"
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

  log_write("initialising pmm, vmm, heap", KERNEL, LOG_INFO);
  pmm_init(mb2_addr);
  vmm_init();
  heap_init();
  log_write("pmm, vmm, heap initialised", KERNEL, LOG_INFO);

  log_write("initialising framebuffer", KERNEL, LOG_INFO);
  framebuffer_init(mb2_addr);
  log_write("framebuffer initialised", KERNEL, LOG_INFO);

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
    log_write_hex("rootfs: root sector phys =",
                  (uint64_t)m->mod_start + 65 * 512, FILESYS, LOG_INFO);
    log_write("rootfs: module found", FILESYS, LOG_INFO);
    log_write("rootfs: initialising fat16 driver", FILESYS, LOG_INFO);
    fat_init((uint8_t *)(uint64_t)m->mod_start, m->mod_end - m->mod_start);
    log_write("rootfs: fat initialised", FILESYS, LOG_INFO);

    log_write("rootfs: reading README.TXT", FILESYS, LOG_INFO);
    struct fat_file f;
    if (fat_open("README.TXT", &f) == 0) {
      char buf[128];
      size_t n = fat_read(&f, buf, sizeof(buf) - 1);
      buf[n] = 0;
      log_write(buf, SYSTEM, LOG_INFO);
      log_write("rootfs: read README.TXT", FILESYS, LOG_INFO);
    } else {
      log_write("rootfs: failed to open README.TXT", FILESYS, LOG_ERROR);
    }
  }

  pic_clear_mask(0);
  pic_clear_mask(1);
  __asm__ volatile("sti");
  log_write("interrupts enabled", KERNEL, LOG_INFO);

  log_write("kernel booted", KERNEL, LOG_INFO);

  {
    pic_clear_mask(0);
    pic_clear_mask(1);

    /* run shell forever; shell-launched children return via process_exec */
    char *sh_argv[] = { (char*)"sh", 0 };
    while (1) {
        long code = process_exec("SH.ELF", sh_argv);
        log_write_hex("shell exited code =", code, KERNEL, LOG_INFO);
    }
  };

  for (;;)
    __asm__ volatile("hlt");
}
