#include "console.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

/* Thin shim over SYS_CON_WRITE / SYS_CON_CLEAR. Glyph rendering lives in the
 * kernel WM now; userspace just streams characters. Color is a kernel concern. */

void console_init(void) {
    /* No state to initialise. */
}

void console_clear(void) {
    con_clear();
}

void console_set_color(uint32_t fg, uint32_t bg) {
    (void)fg;
    (void)bg;
}

void console_putc(char c) {
    con_write(&c, 1);
}

void console_puts(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    con_write(s, n);
}

void console_write(const char *buf, size_t len) {
    con_write(buf, len);
}

int console_save(void) {
    return (int)con_push();
}

int console_restore(void) {
    return (int)con_pop();
}
