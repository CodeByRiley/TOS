#include "display/tty.h"
#include "display/framebuffer.h"
#include "display/font8x8.h"
#include "memory/heap.h"
#include "msg/msg.h"
#include "sched/sched.h"
#include "utilities/string.h"
#include "utilities/log.h"
#include <stdint.h>

#define TTY_FG 0x00FFFFFFu
#define TTY_BG 0x00008080u   /* teal — matches the old WM desktop colour */

#define TTY_PAD 4
#define DRAIN_RING_SIZE 4096
#define DRAIN_RING_MASK (DRAIN_RING_SIZE - 1)
_Static_assert((DRAIN_RING_SIZE & DRAIN_RING_MASK) == 0, "ring must be pow2");

static int     cols, rows;
static int     cx, cy;
static char   *grid;          /* cols * rows */
static int     dirty;
static int     active = 1;
static int     ready  = 0;
static int     px_w, px_h, stride;
static uint32_t *fb;

/* Pre-expanded glyph row: byte → 8 pixels of TTY_FG/TTY_BG. Built once in
 * tty_init from TTY_FG/TTY_BG. 8 KiB BSS. Per-row glyph draw becomes a
 * single memcpy(32 B) instead of 8 conditional pixel writes. */
static uint32_t glyph_lut[256][FONT_GLYPH_W];

/* Userspace winman can drain raw chars via tty_drain so it renders its own
 * console window. Independent from the grid (which only matters when the
 * kernel is doing its own drawing). */
static char    drain_buf[DRAIN_RING_SIZE];
static volatile int drain_head = 0;
static volatile int drain_tail = 0;

static void build_glyph_lut(void) {
    for (int b = 0; b < 256; b++) {
        for (int c = 0; c < FONT_GLYPH_W; c++) {
            glyph_lut[b][c] = ((b >> c) & 1) ? TTY_FG : TTY_BG;
        }
    }
}

static void draw_glyph(int gx, int gy, char c) {
    if (gx < 0 || gy < 0 || gx >= cols || gy >= rows) return;
    int px = TTY_PAD + gx * FONT_GLYPH_W;
    int py = TTY_PAD + gy * FONT_GLYPH_H;
    if (px + FONT_GLYPH_W > px_w || py + FONT_GLYPH_H > px_h) return;

    const uint8_t *glyph;
    static const uint8_t blank[FONT_GLYPH_H] = {0};
    if (c < FONT_FIRST || c > FONT_LAST) glyph = blank;
    else                                 glyph = font8x8[(int)c - FONT_FIRST];

    uint8_t *fbp = (uint8_t*)fb + (uint32_t)py * (uint32_t)stride;
    for (int r = 0; r < FONT_GLYPH_H; r++) {
        uint32_t *row = (uint32_t*)fbp + px;
        memcpy(row, glyph_lut[glyph[r]], FONT_GLYPH_W * sizeof(uint32_t));
        fbp += stride;
    }
}

static void render(void) {
    if (!ready || !active) return;
    framebuffer_clear(TTY_BG);   /* clear marks full damage on its own */
    for (int gy = 0; gy < rows; gy++) {
        const char *gr = grid + gy * cols;
        for (int gx = 0; gx < cols; gx++) {
            char c = gr[gx];
            if (c) draw_glyph(gx, gy, c);
        }
    }
    dirty = 0;
}

static void scroll_one(void) {
    memmove(grid, grid + cols, (size_t)(rows - 1) * (size_t)cols);
    memset(grid + (rows - 1) * cols, 0, (size_t)cols);
}

static void newline(void) {
    cx = 0;
    cy++;
    if (cy >= rows) { scroll_one(); cy = rows - 1; }
}

static void drain_push(char c) {
    int next = (drain_head + 1) & DRAIN_RING_MASK;
    if (next == drain_tail) {
        /* Drop oldest to keep the newest. */
        drain_tail = (drain_tail + 1) & DRAIN_RING_MASK;
    }
    drain_buf[drain_head] = c;
    drain_head = next;
}

void tty_init(void) {
    px_w   = (int)framebuffer_width();
    px_h   = (int)framebuffer_height();
    stride = (int)framebuffer_pitch();
    fb     = framebuffer_buffer();

    cols = (px_w - 2 * TTY_PAD) / FONT_GLYPH_W;
    rows = (px_h - 2 * TTY_PAD) / FONT_GLYPH_H;
    if (cols <= 0 || rows <= 0) {
        log_write("tty: invalid grid dims", KERNEL, LOG_ERROR);
        return;
    }
    grid = (char*)kmalloc((size_t)cols * (size_t)rows);
    if (!grid) {
        log_write("tty: grid kmalloc failed", KERNEL, LOG_ERROR);
        return;
    }
    memset(grid, 0, (size_t)cols * (size_t)rows);
    build_glyph_lut();
    cx = cy = 0;
    dirty = 1;
    active = 1;
    ready  = 1;
    render();
}

void tty_resize(void) {
    if (!ready) return;
    int new_px_w   = (int)framebuffer_width();
    int new_px_h   = (int)framebuffer_height();
    int new_stride = (int)framebuffer_pitch();
    uint32_t *new_fb = framebuffer_buffer();

    int new_cols = (new_px_w - 2 * TTY_PAD) / FONT_GLYPH_W;
    int new_rows = (new_px_h - 2 * TTY_PAD) / FONT_GLYPH_H;
    if (new_cols <= 0 || new_rows <= 0) {
        log_write("tty: invalid resized dims", KERNEL, LOG_ERROR);
        return;
    }

    px_w = new_px_w; px_h = new_px_h; stride = new_stride; fb = new_fb;

    /* Reallocate the character grid only when shape actually changed.
     * Preserve as much of the previous content as fits — copy the bottom
     * min(old_rows, new_rows) rows so the most recent output stays visible. */
    if (new_cols != cols || new_rows != rows) {
        char *new_grid = (char*)kmalloc((size_t)new_cols * (size_t)new_rows);
        if (!new_grid) {
            log_write("tty: resize kmalloc failed", KERNEL, LOG_ERROR);
            return;
        }
        memset(new_grid, 0, (size_t)new_cols * (size_t)new_rows);
        if (grid) {
            int copy_cols = cols < new_cols ? cols : new_cols;
            int copy_rows = rows < new_rows ? rows : new_rows;
            int src_y0 = rows - copy_rows;
            int dst_y0 = new_rows - copy_rows;
            for (int r = 0; r < copy_rows; r++) {
                memcpy(new_grid + (dst_y0 + r) * new_cols,
                       grid     + (src_y0 + r) * cols,
                       (size_t)copy_cols);
            }
            kfree(grid);
        }
        grid = new_grid;
        cols = new_cols;
        rows = new_rows;
        if (cx >= cols) cx = cols - 1;
        if (cy >= rows) cy = rows - 1;
    }
    dirty = 1;
}

void tty_putc(char c) {
    if (!ready) return;
    drain_push(c);
    if (c == '\n') { newline(); dirty = 1; return; }
    if (c == '\r') { cx = 0; dirty = 1; return; }
    if (c == '\b') {
        if (cx > 0) {
            cx--;
            grid[cy * cols + cx] = 0;
            dirty = 1;
        }
        return;
    }
    if (c == '\t') {
        do { tty_putc(' '); } while (cx % 8);
        return;
    }
    if (cx >= cols) newline();
    grid[cy * cols + cx] = c;
    cx++;
    dirty = 1;
}

void tty_write(const char *buf, size_t n) {
    if (!buf) return;
    for (size_t i = 0; i < n; i++) tty_putc(buf[i]);
}

void tty_clear(void) {
    if (!ready) return;
    memset(grid, 0, (size_t)cols * (size_t)rows);
    cx = cy = 0;
    dirty = 1;
    /* Also notify drain consumers (winman) so their mirror clears too. */
    drain_push(TTY_CTRL_CLEAR);
}

/* One-level alt-screen save/restore. Stack-of-1 — repeated tty_push without
 * a tty_pop frees the previous snapshot to keep memory bounded. */
static char *saved_grid = 0;
static int   saved_cx   = 0;
static int   saved_cy   = 0;

int tty_push(void) {
    if (!ready) return -1;
    size_t bytes = (size_t)cols * (size_t)rows;
    if (saved_grid) { kfree(saved_grid); saved_grid = 0; }
    saved_grid = (char*)kmalloc(bytes);
    if (!saved_grid) return -1;
    memcpy(saved_grid, grid, bytes);
    saved_cx = cx;
    saved_cy = cy;
    memset(grid, 0, bytes);
    cx = cy = 0;
    dirty = 1;
    drain_push(TTY_CTRL_PUSH);
    return 0;
}

int tty_pop(void) {
    if (!ready || !saved_grid) return -1;
    size_t bytes = (size_t)cols * (size_t)rows;
    memcpy(grid, saved_grid, bytes);
    cx = saved_cx;
    cy = saved_cy;
    kfree(saved_grid);
    saved_grid = 0;
    dirty = 1;
    drain_push(TTY_CTRL_POP);
    return 0;
}

void tty_set_active(int on) {
    if (active == on) return;
    active = on;
    if (active) {
        dirty = 1;
        render();
    }
}

int tty_is_active(void) { return active; }

size_t tty_drain(char *out, size_t max) {
    size_t n = 0;
    while (n < max && drain_tail != drain_head) {
        out[n++] = drain_buf[drain_tail];
        drain_tail = (drain_tail + 1) & DRAIN_RING_MASK;
    }
    return n;
}

/* Compositor kthread for the kernel TTY. Sleeps a few ticks between
 * frames — the kernel TTY is a fallback, not a game. Also tracks the
 * input owner so we automatically resume drawing when a winman
 * process dies and surrenders ownership.
 *
 * Doubles as the virtio-gpu pump: each iteration polls the device for a
 * host window-resize event, reflows the grid if one arrived, and pushes
 * the backbuffer to the scanout when we drew something. In MB2 mode both
 * calls degrade to no-ops, so the thread behaves identically there. */
void tty_thread_entry(void) {
    for (;;) {
        if (framebuffer_check_resize()) tty_resize();

        int owner = msg_input_owner();
        int want_active = (owner == 0);
        if (want_active != active) tty_set_active(want_active);

        if (ready && active && dirty) render();
        /* Flush is owned by framebuffer_flush_thread_entry now — it polls
         * the damage rect at PIT tick rate independently of the tty render
         * cadence so a slow virtio ACK can't stall this thread. */

        task_sleep_ticks(3);
    }
}
