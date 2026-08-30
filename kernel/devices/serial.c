/* kernel/devices/serial.c , COM1 UART driver.
 *
 * 38400 8N1, polled output. Used for kernel printf + early-boot logs
 * (visible in QEMU's serial window). Loopback self-test on init catches
 * a missing UART before the rest of the boot continues to lean on it.
 */
#include <devices/serial.h>
#include <display/print.h>
#include <devices/io.h>

#define COM1 0x3F8

/* Program the UART and run a loopback self-test. Returns 0 on success
 * (UART present + responding), 1 if the loopback byte didn't echo. */
int serial_init(void) {
    print_newline();
    print_write_str("serial_init: starting\n");
    outb(COM1 + 1, 0x00); /* disable interrupts                        */
    outb(COM1 + 3, 0x80); /* DLAB on                                    */
    outb(COM1 + 0, 0x03); /* divisor low  = 3 (38400)                  */
    outb(COM1 + 1, 0x00); /* divisor high = 0                          */
    outb(COM1 + 3, 0x03); /* 8N1, DLAB off                              */
    outb(COM1 + 2, 0xC7); /* FIFO enable, clear, 14-byte threshold     */
    outb(COM1 + 4, 0x0B); /* RTS/DSR set, OUT2 (IRQ enable line)       */
    outb(COM1 + 4, 0x1E); /* loopback test mode                         */
    outb(COM1 + 0, 0xAE); /* send sentinel byte                         */
    if (inb(COM1 + 0) != 0xAE) {
        print_write_str("serial_init: loopback test failed\n");
        return 1;
    }
    outb(COM1 + 4, 0x0F); /* back to normal mode                        */
    print_write_str("serial_init: loopback test passed\n");
    return 0;
}

/* Bit 5 of LSR = transmitter holding-register empty. */
static int tx_empty(void) { return inb(COM1 + 5) & 0x20; }

/* Polled char write , busy-waits for THR-empty. */
void serial_write_char(char c) {
    while (!tx_empty()) {
    }
    outb(COM1, (u8)c);
}

/* Polled string write. Translates LF → CRLF so terminals render newlines
 * correctly without "stairstepping". */
void serial_write_str(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_write_char('\r');
        serial_write_char(*s++);
    }
}

/* "0x" + 16 hex digits, big-endian. */
void serial_write_hex(u64 n) {
    serial_write_str("0x");
    for (int i = 60; i >= 0; i -= 4) {
        u8 nib = (n >> i) & 0xF;
        serial_write_char(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
}
