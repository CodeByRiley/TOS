
#include "devices/pit.h"
#include "devices/io.h"
#include "interrupts/idt.h"
#include "devices/serial.h"
#include "sched/sched.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_FREQ 1193182

static volatile uint64_t ticks = 0;

/* PIT IRQ stays cheap: bump tick counter, wake any sleepers whose deadline
 * has passed. Heavy work (compositor, scheduler decisions) belongs in a
 * kthread, not IRQ context. */
static void pit_handler(void) {
    ticks++;
    struct task *cur = task_current();
    if (cur) cur->ticks_run++;
    sched_wake_sleepers();
}

void pit_init(uint32_t freq_hz) {
    uint32_t div = PIT_FREQ / freq_hz;
    outb(PIT_CMD, 0x36);                // channel 0, lo/hi byte, mode 3, binary
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
    irq_install(0, pit_handler);
}

uint64_t pit_ticks(void) { return ticks; }
