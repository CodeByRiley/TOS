/* kernel/utilities/panic.c — see panic.h. */
#include "utilities/panic.h"
#include "devices/serial.h"
#include "utilities/log.h"
#include "utilities/printf.h"

void panic_at(const char *msg, const char *file, int line, const char *func) {
  /* Interrupts off first: the log path touches shared state, and taking a
   * timer tick (or a nested fault) mid-panic loses the message we came here
   * to print. */
  __asm__ volatile ("cli");

  /* Static, not stack — a panic caused by stack exhaustion still has to be
   * able to format its own message. Single-shot by nature, so no reentrancy
   * concern. */
  static char buf[256];
  snprintf(buf, sizeof(buf), "PANIC: %s:%d %s(): %s", file, line, func, msg);

  /* Serial first, unconditionally. log_write also reaches the framebuffer,
   * which may itself be what's broken. */
  serial_write_str(buf);
  serial_write_char('\n');
  log_write(buf, KERNEL, LOG_FATAL);

  for (;;)
    __asm__ volatile ("cli; hlt");
}
