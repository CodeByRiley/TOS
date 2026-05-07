#include "boot/multiboot2.h"
#include "devices/pit.h"
#include "devices/serial.h"
#include "display/framebuffer.h"
#include "display/print.h"
#include "fs/fat.h"
#include "input/keyboard.h"
#include "interrupts/idt.h"
#include "interrupts/pic.h"
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

  struct MB2_TAG_MODULE *m = mb2_find_module(mb2_addr, "rootfs");
  if (!m) {
    log_write("rootfs: no rootfs module", KERNEL, LOG_ERROR);
  } else {
    log_write_hex("rootfs: module phys =", (uint64_t)m->mod_start, FILE,
                  LOG_INFO);
    log_write_hex("rootfs: module size =", m->mod_end - m->mod_start, FILE,
                  LOG_INFO);
    log_write_hex("rootfs: root sector phys =",
                  (uint64_t)m->mod_start + 65 * 512, FILE, LOG_INFO);
    log_write("rootfs: module found", FILE, LOG_INFO);
    log_write("rootfs: initialising fat16 driver", FILE, LOG_INFO);
    fat_init((uint8_t *)(uint64_t)m->mod_start, m->mod_end - m->mod_start);
    log_write("rootfs: fat initialised", FILE, LOG_INFO);

    log_write("rootfs: reading README.TXT", FILE, LOG_INFO);
    struct fat_file f;
    if (fat_open("README.TXT", &f) == 0) {
      char buf[128];
      size_t n = fat_read(&f, buf, sizeof(buf) - 1);
      buf[n] = 0;
      log_write(buf, SYSTEM, LOG_INFO);
      log_write("rootfs: read README.TXT", FILE, LOG_INFO);
    } else {
      log_write("rootfs: failed to open README.TXT", FILE, LOG_ERROR);
    }
  }


  // smoke test
  uint64_t a = pmm_alloc_frame();
  uint64_t b = pmm_alloc_frame();
  log_write_hex("alloc a =", a, SYSTEM, LOG_INFO);
  log_write_hex("alloc b =", b, SYSTEM, LOG_INFO);
  pmm_free_frame(a);
  uint64_t c = pmm_alloc_frame();
  log_write_hex("alloc c =", c, SYSTEM, LOG_INFO);

  void *p = kmalloc(64);
  log_write_hex("kmalloc 64 =", (uint64_t)p, SYSTEM, LOG_INFO);
  kfree(p);

  pic_clear_mask(0);
  pic_clear_mask(1);
  __asm__ volatile("sti");
  log_write("interrupts enabled", KERNEL, LOG_INFO);

  log_write("kernel booted", KERNEL, LOG_INFO);

  log_write("gradient test", KERNEL, LOG_INFO);
  // gradient test
  for (uint32_t y = 0; y < framebuffer_height(); y++) {
    for (uint32_t x = 0; x < framebuffer_width(); x++) {
      uint8_t r = x * 255 / framebuffer_width();
      uint8_t g = y * 255 / framebuffer_height();
      uint8_t b = 128;
      framebuffer_putpixel(x, y, (r << 16) | (g << 8) | b);
    }
  }


  printf("hello %s, num=%d hex=%x ptr=%p\n", "world", -42, 0xdead, kernel_main);
  for (;;)
    __asm__ volatile("hlt");
}
