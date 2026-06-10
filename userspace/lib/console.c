/* userspace/lib/console.c — userspace console wrappers.
 *
 * Thin shims over SYS_CON_WRITE / SYS_CON_CLEAR / SYS_CON_PUSH / SYS_CON_POP.
 * Glyph rendering and palette live in the kernel WM; userspace only streams
 * characters and toggles the alt-screen stack.
 */
#include "console.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

/* No client-side state to set up. */
void console_init(void) {
}

/* Clear visible TTY contents. */
void console_clear(void) {
    con_clear();
}

/* Color is a kernel concern — stub kept so apps can call it portably. */
void console_set_color(uint32_t fg, uint32_t bg) {
    (void)fg;
    (void)bg;
}

/* Write a single character. */
void console_putc(char c) {
    con_write(&c, 1);
}

/* Write a NUL-terminated string (uses inline strlen to avoid libc dep). */
void console_puts(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    con_write(s, n);
}

/* Write `len` bytes of `buf`, NUL-permitting. */
void console_write(const char *buf, size_t len) {
    con_write(buf, len);
}

/* Snapshot screen + cursor onto the kernel's 1-deep stack and clear. */
int console_save(void) {
    return (int)con_push();
}

/* Restore the previously saved screen. */
int console_restore(void) {
    return (int)con_pop();
}
