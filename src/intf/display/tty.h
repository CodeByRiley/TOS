#ifndef TTY_H
#define TTY_H

#include <stddef.h>
#include <stdint.h>

/* In-band control codes emitted into drain_buf so out-of-tree consumers
 * (winman's console window) can mirror tty_clear / tty_push / tty_pop on
 * their own surfaces. Picked from low unassigned C0 ASCII codes so they
 * don't collide with regular text or with handled control chars
 * (\t \n \r \b are already in use). */
#define TTY_CTRL_CLEAR  0x0C   /* form-feed: wipe surface, home cursor   */
#define TTY_CTRL_PUSH   0x1C   /* save+clear (alt-screen enter)          */
#define TTY_CTRL_POP    0x1D   /* restore (alt-screen exit)              */

/* Simple framebuffer-backed text terminal. Lives in the kernel as a
 * fallback renderer when no userspace window manager is active. When the
 * userspace winman registers itself it calls tty_set_active(0) and stops
 * the kernel from touching the framebuffer; on winman exit the kernel
 * re-enables the TTY so the shell-fallback boot path stays visible. */

void tty_init(void);
void tty_putc(char c);
void tty_write(const char *buf, size_t n);
void tty_clear(void);

/* Disable framebuffer drawing while winman owns the screen. Characters
 * still buffer into the grid so the kernel log/console doesn't blackhole. */
void tty_set_active(int on);
int  tty_is_active(void);

/* Drain `max` chars of unconsumed input into `out`. Returns number copied.
 * Used by userspace winman so it can render the kernel's text stream into
 * a winman-owned console window. */
size_t tty_drain(char *out, size_t max);

/* Alt-screen save/restore — one level deep. tty_push snapshots the current
 * grid + cursor into a backing buffer then clears the live grid so a full-
 * screen app (btop, etc.) draws on a fresh canvas. tty_pop restores the
 * snapshot. No-op if push wasn't called or restore already happened. */
int  tty_push(void);
int  tty_pop(void);

void tty_thread_entry(void);

/* Recompute the grid for the current framebuffer dimensions. Reallocates the
 * backing grid if cols/rows changed and forces a redraw. Called by the
 * render loop after framebuffer_check_resize signals a host-driven resize. */
void tty_resize(void);

#endif
