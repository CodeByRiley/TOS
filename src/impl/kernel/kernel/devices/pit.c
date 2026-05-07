
#include "devices/pit.h"
#include "arch/io.h"
#include "interrupts/idt.h"
#include "devices/serial.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_FREQ 1193182

static volatile uint64_t ticks = 0;

static void pit_handler(void) {
    ticks++;
    // if (ticks % 100 == 0) {
    //     serial_write_str("tick ");
    //     serial_write_hex(ticks);
    //     serial_write_str("\n");
    // }
}

void pit_init(uint32_t freq_hz) {
    uint32_t div = PIT_FREQ / freq_hz;
    outb(PIT_CMD, 0x36);                // channel 0, lo/hi byte, mode 3, binary
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
    irq_install(0, pit_handler);
}

uint64_t pit_ticks(void) { return ticks; }
