/* userspace/lib/console.h — userspace console API.
 *
 * Thin wrapper over SYS_CON_* syscalls. Apps stream text through the
 * kernel TTY; the kernel WM handles glyph rendering and color. The
 * save/restore pair lets fullscreen apps (btop, etc.) snapshot the TTY,
 * draw on a cleared screen, and put the original screen back on exit.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* No-op kept for symmetry; nothing to initialise client-side. */
void console_init(void);

/* Erase the visible TTY (SYS_CON_CLEAR). */
void console_clear(void);

/* Emit a single character to the TTY. */
void console_putc(char c);

/* Emit a NUL-terminated string to the TTY. */
void console_puts(const char *s);

/* Emit `len` bytes of `buf` to the TTY. May contain embedded NULs. */
void console_write(const char *buf, size_t len);

/* No-op stub: color is owned by the kernel WM, not the client. */
void console_set_color(uint32_t fg, uint32_t bg);

/* Push the current TTY screen + cursor on a 1-deep stack and clear. */
int  console_save(void);

/* Pop the screen previously saved by console_save(). */
int  console_restore(void);

#endif
