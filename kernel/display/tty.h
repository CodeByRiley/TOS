/* kernel/display/tty.h — framebuffer-backed text terminal.
 *
 * Kernel-side fallback console. When the userspace winman registers it
 * calls tty_set_active(0), stops the kernel from touching the
 * framebuffer, and starts draining the TTY into its own console window;
 * on winman exit the kernel re-enables the TTY so the shell-fallback
 * boot path stays visible.
 *
 * In-band control codes are emitted into drain_buf so winman can mirror
 * tty_clear / tty_push / tty_pop on its surface. They sit in unassigned
 * low C0 ASCII so they don't collide with regular text or handled
 * control chars (\t \n \r \b are already in use).
 *
 * Implementation: kernel/display/tty.c.
 */
#ifndef TTY_H
#define TTY_H

#include <stddef.h>
#include <stdint.h>

#define TTY_CTRL_CLEAR  0x0C   /* form-feed: wipe surface, home cursor   */
#define TTY_CTRL_PUSH   0x1C   /* save+clear (alt-screen enter)          */
#define TTY_CTRL_POP    0x1D   /* restore (alt-screen exit)              */
#define TTY_CTRL_ZOOM_IN  0x1E /* increase glyph scale by one step       */
#define TTY_CTRL_ZOOM_OUT 0x1F /* decrease glyph scale by one step       */

void tty_init(void);
void tty_putc(char c);
void tty_write(const char *buf, size_t n);
void tty_clear(void);

/* Disable framebuffer drawing while winman owns the screen. Text still
 * buffers into the grid so the kernel log doesn't blackhole. */
void tty_set_active(int on);
int  tty_is_active(void);

/* Drain up to `max` chars of unconsumed input into `out`. Returns count.
 * Used by userspace winman to render the kernel text stream into its own
 * console window. */
size_t tty_drain(char *out, size_t max);

/* Alt-screen save/restore — single level. push snapshots the grid +
 * cursor into a backing buffer then clears the live grid so a fullscreen
 * app draws on a fresh canvas; pop restores. No-op if push wasn't called
 * or restore already happened. */
int  tty_push(void);
int  tty_pop(void);

/* Change the glyph scale by one step in the sign of delta. Returns the
 * resulting scale (1..4), or -1 when the grid cannot be reallocated. */
int  tty_zoom(int delta);

void tty_thread_entry(void);

/* Re-allocate the grid for the current framebuffer dimensions. Called by
 * the render loop after framebuffer_check_resize signals a host resize. */
void tty_resize(void);

#endif
