/* kernel/utilities/printf.h — kernel printf surface.
 *
 * vsnprintf/snprintf/printf live in kernel/utilities/printf.c.
 * The format engine matches the userspace one (full C99 conversion spec
 * set: flags - + # 0 space, width, precision, length hh/h/l/ll/z/j/t,
 * conversions d/i/u/o/x/X/p/s/c/%). printf() writes to the serial port.
 */
#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>
#include <stddef.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int printf(const char *fmt, ...);

#endif
