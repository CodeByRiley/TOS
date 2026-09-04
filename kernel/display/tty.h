/* kernel/display/tty.h , framebuffer-backed text terminals.
 *
 * There are TTY_MAX channels. Each owns an input ring (keystrokes on their
 * way to a shell) and a drain ring (output on its way to whoever renders
 * it). A task's channel is task->tty, inherited on spawn, so every process
 * in one shell's tree shares that shell's terminal and no other.
 *
 * Channel TTY_KERNEL (0) is special in one way only: it is the one the
 * kernel itself renders. It owns the character grid, the glyph scale and
 * the framebuffer blit, so the boot shell stays visible before winman
 * exists and again if winman dies. When winman registers it calls
 * tty_set_active(0), which stops the blitting; text still accumulates in
 * the grid so the kernel log does not blackhole.
 *
 * Channels 1..TTY_MAX-1 are drain-only: no grid, no rendering, nothing but
 * the two rings. They exist to be mirrored by a userspace console window,
 * so they are only useful while something is draining them. tty_alloc()
 * hands one out and tty_release() gives it back.
 *
 * In-band control codes are emitted into the drain ring so winman can
 * mirror tty_clear_ch / tty_push_ch / tty_pop_ch on its surface. They sit in
 * unassigned low C0 ASCII so they don't collide with regular text or
 * handled control chars (\t \n \r \b are already in use).
 *
 * Implementation: kernel/display/tty.c.
 */
#ifndef TTY_H
#define TTY_H

#include <utilities/types.h>
#include <stddef.h>
#include <stdint.h>

#define TTY_CTRL_CLEAR  0x0C   /* form-feed: wipe surface, home cursor   */
#define TTY_CTRL_PUSH   0x1C   /* save+clear (alt-screen enter)          */
#define TTY_CTRL_POP    0x1D   /* restore (alt-screen exit)              */
#define TTY_CTRL_ZOOM_IN  0x1E /* increase glyph scale by one step       */
#define TTY_CTRL_ZOOM_OUT 0x1F /* decrease glyph scale by one step       */

/* Channel count. Userspace winman mirrors one console window per channel,
 * so CON_MAX in userspace/bin/winman/winman.h must not exceed this. */
#define TTY_MAX     4
#define TTY_KERNEL  0

extern struct ttf_font *g_sys_font;

void tty_init(void);

/* Disable framebuffer drawing while winman owns the screen. Text still
 * buffers into the grid so the kernel log doesn't blackhole. */
void tty_set_active(int on);
int  tty_is_active(void);

/* --- channel-addressed forms ------------------------------------------
 *
 * Every operation names its channel explicitly. Kernel output passes
 * TTY_KERNEL; syscall paths pass the caller's task->tty.
 *
 * Out-of-range or closed channels are ignored (writes) or return 0 / -1
 * (reads), so a stale index from userspace cannot corrupt another channel. */
void   tty_write_ch(int idx, const char *buf, usize n);
void   tty_clear_ch(int idx);
int    tty_push_ch(int idx);
int    tty_pop_ch(int idx);
int    tty_zoom_ch(int idx, int delta);
void   tty_inject_input_ch(int idx, char c);
usize tty_read_input_ch(int idx, char *buf, usize max);
usize tty_drain_ch(int idx, char *out, usize max);

/* Claim a free channel in 1..TTY_MAX-1, empty its rings, and return its
 * index. Returns -1 when every channel is taken. TTY_KERNEL is never
 * handed out: it belongs to the kernel for the lifetime of the boot. */
int  tty_alloc(void);

/* Give a channel back. Discards anything still buffered on it. No-op for
 * TTY_KERNEL and for a channel that is already free. */
void tty_release(int idx);

/* True if `idx` names a channel that is currently open. */
int  tty_is_open(int idx);

void tty_thread_entry(void);

/* Re-allocate the grid for the current framebuffer dimensions. Called by
 * the render loop after framebuffer_check_resize signals a host resize. */
void tty_resize(void);

#endif
