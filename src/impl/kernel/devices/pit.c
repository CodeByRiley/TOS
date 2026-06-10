/* src/impl/kernel/devices/pit.c — 8254 PIT driver.
 *
 * Channel 0 fires IRQ0 at the requested frequency. The handler bumps the
 * global tick counter, charges the current task for the slice, and walks
 * sleepers. Anything heavier (compositor, scheduler decisions) belongs
 * in a kthread, not IRQ context.
 */
#include "devices/pit.h"
#include "devices/io.h"
#include "interrupts/idt.h"
#include "devices/serial.h"
#include "sched/sched.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_FREQ 1193182        /* 8254 input clock — divides down */

static volatile uint64_t ticks = 0;

/* IRQ0 handler. Kept cheap on purpose. */
static void pit_handler(void) {
    ticks++;
    struct task *cur = task_current();
    if (cur) cur->ticks_run++;
    sched_wake_sleepers();
}

/* Program PIT channel 0 to fire at `freq_hz`. */
void pit_init(uint32_t freq_hz) {
    uint32_t div = PIT_FREQ / freq_hz;
    outb(PIT_CMD, 0x36);                /* channel 0, lo/hi byte, mode 3, binary */
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
    irq_install(0, pit_handler);
}

uint64_t pit_ticks(void) { return ticks; }
