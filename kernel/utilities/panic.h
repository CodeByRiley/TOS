/* kernel/utilities/panic.h , unrecoverable-error surface.
 *
 * panic() emits a structured serial report with the call site, CPU/task and
 * bounded backtrace, draws a restart-style framebuffer screen when possible,
 * and then halts this CPU for good. Use it for invariant violations the
 * kernel cannot continue past.
 *
 * Implementation: kernel/utilities/panic.c.
 */
#ifndef PANIC_H
#define PANIC_H

#include <utilities/types.h>
#include <utilities/symtab.h>
#include <devices/io.h>

struct interrupt_frame;

/* Never returns. Prefer the panic() macro so the call site fills itself in. */
NORETURN
void panic_at(const char *msg, const char *file, int line, const char *func);

/* Fatal CPU exception entry. The IDT recovery path handles recoverable probe
 * faults before calling this, so this function never returns. */
NORETURN
void panic_from_exception(const char *name,
                          const struct interrupt_frame *frame,
                          u64 fault_address,
                          int has_fault_address);

#define panic(msg) panic_at((msg), __FILE__, __LINE__, __func__)

/* Guard for code that cannot make forward progress with interrupts off.
 * Callers that sleep, yield, or wait on pit_ticks() belong here: without IRQ0
 * they hang the machine with no output, so fail loudly and name the function
 * instead. Busy-waits driven by polled hardware (see pit_delay_us) are exempt
 * , they are the correct thing to call in that window. */
#define REQUIRE_INTERRUPTS()                                                   \
  do {                                                                         \
    if (!interrupts_enabled())                                                 \
      panic("called with interrupts disabled");                                \
  } while (0)

#endif
