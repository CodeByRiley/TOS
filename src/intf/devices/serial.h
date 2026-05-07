#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
int serial_init(void);
void serial_write_char(char c);
void serial_write_str(const char *str);
void serial_write_hex(uint64_t hex);

#endif
