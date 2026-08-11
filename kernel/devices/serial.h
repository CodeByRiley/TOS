/* kernel/devices/serial.h — COM1 UART driver.
 *
 * Used for kernel printf + early-boot logging (visible in QEMU's serial
 * window). 38400 8N1, polled I/O.
 *
 * Implementation: kernel/devices/serial.c.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* Probe + init COM1. Returns 0 if a UART responded, non-zero otherwise. */
int  serial_init(void);

/* Polled writers. */
void serial_write_char(char c);
void serial_write_str(const char *str);

/* Format `hex` as "0x%016lx" and write. */
void serial_write_hex(uint64_t hex);

#endif
