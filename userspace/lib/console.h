#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_puts(const char *s);
void console_write(const char *buf, size_t len);
void console_set_color(uint32_t fg, uint32_t bg);

#endif
