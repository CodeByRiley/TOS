/* kernel/utilities/panic.h — unrecoverable-error surface.
 *
 * panic() logs at LOG_FATAL with the call site attached and then halts this
 * CPU for good. Use it for invariant violations the kernel cannot continue
 * past — the alternative is usually a silent hang, which is far harder to
 * diagnose from a boot log.
 *
 * Implementation: kernel/utilities/panic.c.
 */
#ifndef PANIC_H
#define PANIC_H

#include "devices/io.h"

/* Never returns. Prefer the panic() macro so the call site fills itself in. */
__attribute__((noreturn))
void panic_at(const char *msg, const char *file, int line, const char *func);

#define panic(msg) panic_at((msg), __FILE__, __LINE__, __func__)

/* Guard for code that cannot make forward progress with interrupts off.
 * Callers that sleep, yield, or wait on pit_ticks() belong here: without IRQ0
 * they hang the machine with no output, so fail loudly and name the function
 * instead. Busy-waits driven by polled hardware (see pit_delay_us) are exempt
 * — they are the correct thing to call in that window. */
#define REQUIRE_INTERRUPTS()                                                   \
  do {                                                                         \
    if (!interrupts_enabled())                                                 \
      panic("called with interrupts disabled");                                \
  } while (0)

#endif
