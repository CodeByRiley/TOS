/* kernel/display/tty.c , framebuffer-backed text terminal.
 *
 * Kernel-side fallback console. Renders an 8x8 bitmap font into the
 * framebuffer via the font8x8 table. Push/pop give a single-level alt-
 * screen; the drain ring buffers unconsumed text so userspace winman
 * can mirror it into its own console window.
 *
 * Lifecycle: active by default. Userspace winman calls tty_set_active(0)
 * once it registers, after which characters still accumulate in the
 * grid + drain ring but nothing gets blitted. On winman exit, the kernel
 * flips active back on and the shell-fallback path stays visible.
 */
#include <display/tty.h>
#include <display/fonts/ttf.h>
#include <display/framebuffer.h>
#include <display/fonts/font8x8.h>
#include <memory/heap.h>
#include <msg/msg.h>
#include <sched/sched.h>
#include <utilities/string.h>
#include <utilities/log.h>
#include <stdint.h>

#define TTY_FG 0x00FFFFFFu
#define TTY_BG 0x00008080u   /* teal , matches the old WM desktop colour */

#define TTY_PAD 4
#define TTY_SCALE_MIN 1
#define TTY_SCALE_MAX 4
#define DRAIN_RING_SIZE 4096
#define DRAIN_RING_MASK (DRAIN_RING_SIZE - 1)

#define TTF_PX_SIZE  16
#define TTF_CELL_W   9
#define TTF_CELL_H   18

#define TTY_INPUT_SIZE 256

_Static_assert((DRAIN_RING_SIZE & (DRAIN_RING_SIZE - 1)) == 0,
               "drain ring must be pow2");

/* One channel: keystrokes in, text out, nothing else. The grid, the glyph
 * scale and the framebuffer live outside this struct because only channel
 * TTY_KERNEL has them , see the header. */
struct tty_chan {
    char in[TTY_INPUT_SIZE];
    volatile int in_head, in_tail;

    char drain[DRAIN_RING_SIZE];
    volatile int drain_head, drain_tail;

    int open;
};

/* Channel 0 is open before anything runs and is never closed: the kernel
 * log writes to it from early boot, well before tty_init. */
static struct tty_chan chans[TTY_MAX] = {
    [TTY_KERNEL] = { .open = 1 },
};

static int chan_valid(int idx) {
    return idx >= 0 && idx < TTY_MAX && chans[idx].open;
}

static void chan_inject(struct tty_chan *ch, char c) {
    int next = (ch->in_head + 1) % TTY_INPUT_SIZE;
    if (next != ch->in_tail) {
        ch->in[ch->in_head] = c;
        ch->in_head = next;
    }
}

static usize chan_read(struct tty_chan *ch, char *buf, usize max) {
    usize count = 0;
    while (count < max && ch->in_tail != ch->in_head) {
        buf[count++] = ch->in[ch->in_tail];
        ch->in_tail = (ch->in_tail + 1) % TTY_INPUT_SIZE;
    }
    return count;
}

static void chan_drain_push(struct tty_chan *ch, char c) {
    int next = (ch->drain_head + 1) & DRAIN_RING_MASK;
    if (next == ch->drain_tail) {
        /* Drop oldest to keep the newest. */
        ch->drain_tail = (ch->drain_tail + 1) & DRAIN_RING_MASK;
    }
    ch->drain[ch->drain_head] = c;
    ch->drain_head = next;
}

static usize chan_drain(struct tty_chan *ch, char *out, usize max) {
    usize n = 0;
    while (n < max && ch->drain_tail != ch->drain_head) {
        out[n++] = ch->drain[ch->drain_tail];
        ch->drain_tail = (ch->drain_tail + 1) & DRAIN_RING_MASK;
    }
    return n;
}

void tty_inject_input_ch(int idx, char c) {
    if (!chan_valid(idx))
        return;
    chan_inject(&chans[idx], c);
}

usize tty_read_input_ch(int idx, char *buf, usize max) {
    if (!chan_valid(idx) || !buf)
        return 0;
    return chan_read(&chans[idx], buf, max);
}

usize tty_drain_ch(int idx, char *out, usize max) {
    if (!chan_valid(idx) || !out)
        return 0;
    return chan_drain(&chans[idx], out, max);
}

int tty_is_open(int idx) { return chan_valid(idx); }

int tty_alloc(void) {
    for (int i = 1; i < TTY_MAX; i++) {
        if (chans[i].open)
            continue;
        chans[i].in_head = chans[i].in_tail = 0;
        chans[i].drain_head = chans[i].drain_tail = 0;
        chans[i].open = 1;
        return i;
    }
    return -1;
}

void tty_release(int idx) {
    if (idx <= TTY_KERNEL || idx >= TTY_MAX)
        return;
    chans[idx].open = 0;
    chans[idx].in_head = chans[idx].in_tail = 0;
    chans[idx].drain_head = chans[idx].drain_tail = 0;
}

/* Font-derived cell metrics, filled on first TTF draw. The font is
 * proportional but the grid is fixed, so each glyph is centred in its cell;
 * TTF_CELL_W/H are only the fallback if the metrics look nonsensical. */
static int ttf_cell_w = TTF_CELL_W;
static int ttf_cell_h = TTF_CELL_H;
static int ttf_ascent, ttf_descent;
static int ttf_metrics_ready = 0;

static void ttf_measure(void) {
    if (ttf_metrics_ready || !g_sys_font) return;

    ttf_vmetrics(g_sys_font, TTF_PX_SIZE, &ttf_ascent, &ttf_descent, 0);
    if (ttf_ascent <= 0) ttf_ascent = (TTF_PX_SIZE * 3) / 4;

    int w = ttf_cell_width(g_sys_font, TTF_PX_SIZE);
    ttf_cell_w = w > 0 ? w : TTF_CELL_W;
    ttf_cell_h = TTF_PX_SIZE + 3;
    ttf_metrics_ready = 1;
}


static int     cols, rows;
static int     cx, cy;
static int     scale = TTY_SCALE_MIN;
static char   *grid;          /* cols * rows */
static char   *saved_grid;
static int     saved_cx, saved_cy;
static int     dirty;
static int     active = 1;
static int     ready  = 0;
static int     px_w, px_h, stride;
static u32 *fb;

/* Pre-expanded glyph row: byte → 8 pixels of TTY_FG/TTY_BG. Built once in
 * tty_init from TTY_FG/TTY_BG. 8 KiB BSS. Per-row glyph draw becomes a
 * single memcpy(32 B) instead of 8 conditional pixel writes. */
static u32 glyph_lut[256][FONT_GLYPH_W];

/* Modify cell_width and cell_height to check for TTF */
static int cell_width(void) {
    if (g_sys_font) { ttf_measure(); return ttf_cell_w; }
    return FONT_GLYPH_W * scale;
}
static int cell_height(void) {
    if (g_sys_font) { ttf_measure(); return ttf_cell_h; }
    return FONT_GLYPH_H * scale;
}

static void build_glyph_lut(void) {
    for (int b = 0; b < 256; b++) {
        for (int c = 0; c < FONT_GLYPH_W; c++) {
            glyph_lut[b][c] = ((b >> c) & 1) ? TTY_FG : TTY_BG;
        }
    }
}

static void draw_glyph(int gx, int gy, char c) {
    if (gx < 0 || gy < 0 || gx >= cols || gy >= rows) return;
    int cw = cell_width();
    int ch = cell_height();
    int px = TTY_PAD + gx * cw;
    int py = TTY_PAD + gy * ch;
    if (px + cw > px_w || py + ch > px_h) return;

    if (g_sys_font) {
        /* Clear the cell first: unlike the font8x8 path there is no per-pixel
         * background write, so an overwritten or erased cell would otherwise
         * keep the old glyph. */
        for (int y = 0; y < ch; y++) {
            u32 *row = (u32*)((u8*)fb +
                                        (u32)(py + y) * (u32)stride) + px;
            for (int i = 0; i < cw; i++) row[i] = TTY_BG;
        }

        if ((unsigned char)c < 32 || (unsigned char)c > 126 || c == ' ') return;

        struct gfx_surface s;
        gfx_surface_init(&s, fb, px_w, px_h, stride / 4);

        /* Sit the baseline so the ascender/descender band is centred in the
         * cell instead of guessing at 3/4 of the way down. */
        int band = ttf_ascent - ttf_descent;
        int baseline = py + (ch - band) / 2 + ttf_ascent;

        ttf_draw_glyph_cell(&s, g_sys_font, px, baseline, cw,
                            (unsigned char)c, TTF_PX_SIZE, TTY_FG);
        return;
    }

    const u8 *glyph;
    static const u8 blank[FONT_GLYPH_H] = {0};
    if (c < FONT_FIRST || c > FONT_LAST) glyph = blank;
    else                                 glyph = font8x8[(int)c - FONT_FIRST];

    if (scale == 1) {
        u8 *fbp = (u8*)fb + (u32)py * (u32)stride;
        for (int r = 0; r < FONT_GLYPH_H; r++) {
            u32 *row = (u32*)fbp + px;
            memcpy(row, glyph_lut[glyph[r]], FONT_GLYPH_W * sizeof(u32));
            fbp += stride;
        }
        return;
    }

    for (int r = 0; r < FONT_GLYPH_H; r++) {
        for (int sy = 0; sy < scale; sy++) {
            int y = py + r * scale + sy;
            u32 *row = (u32*)((u8*)fb +
                                       (u32)y * (u32)stride) + px;
            for (int cidx = 0; cidx < FONT_GLYPH_W; cidx++) {
                u32 color = ((glyph[r] >> cidx) & 1) ? TTY_FG : TTY_BG;
                for (int sx = 0; sx < scale; sx++)
                    row[cidx * scale + sx] = color;
            }
        }
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
    memmove(grid, grid + cols, (usize)(rows - 1) * (usize)cols);
    memset(grid + (rows - 1) * cols, 0, (usize)cols);
}

static void newline(void) {
    cx = 0;
    cy++;
    if (cy >= rows) { scroll_one(); cy = rows - 1; }
}

/* Everything below this point is the kernel-rendered channel, so its drain
 * pushes always target TTY_KERNEL. Other channels reach their ring through
 * the _ch entry points at the bottom of the file. */
static void drain_push(char c) { chan_drain_push(&chans[TTY_KERNEL], c); }

static void discard_saved_grid(void) {
    if (saved_grid) {
        kfree(saved_grid);
        saved_grid = 0;
    }
}

/* Replace the character grid while preserving the row window containing
 * the cursor. This keeps top-of-screen content during early zooms and the
 * newest rows after the terminal has filled and scrolled. */
static int reflow_grid(int new_cols, int new_rows) {
    if (new_cols <= 0 || new_rows <= 0) return -1;
    if (new_cols == cols && new_rows == rows) return 0;

    char *new_grid = (char*)kmalloc((usize)new_cols * (usize)new_rows);
    if (!new_grid) return -1;
    memset(new_grid, 0, (usize)new_cols * (usize)new_rows);

    int new_cx = 0;
    int new_cy = 0;
    if (grid) {
        int copy_cols = cols < new_cols ? cols : new_cols;
        int copy_rows = rows < new_rows ? rows : new_rows;
        int src_y0 = cy >= copy_rows ? cy - copy_rows + 1 : 0;
        int dst_y0 = 0;
        for (int r = 0; r < copy_rows; r++) {
            memcpy(new_grid + (dst_y0 + r) * new_cols,
                   grid     + (src_y0 + r) * cols,
                   (usize)copy_cols);
        }
        new_cx = cx < new_cols ? cx : new_cols - 1;
        new_cy = cy - src_y0 + dst_y0;
        if (new_cy < 0) new_cy = 0;
        if (new_cy >= new_rows) new_cy = new_rows - 1;
        kfree(grid);
    }

    grid = new_grid;
    cols = new_cols;
    rows = new_rows;
    cx = new_cx;
    cy = new_cy;

    /* A saved grid uses the old dimensions. Discarding it is safer than
     * allowing kernel_pop() to copy a differently sized allocation. */
    discard_saved_grid();
    return 0;
}

void tty_init(void) {
    px_w   = (int)framebuffer_width();
    px_h   = (int)framebuffer_height();
    stride = (int)framebuffer_pitch();
    fb     = framebuffer_buffer();

    scale = TTY_SCALE_MIN;
    cols = (px_w - 2 * TTY_PAD) / cell_width();
    rows = (px_h - 2 * TTY_PAD) / cell_height();
    if (cols <= 0 || rows <= 0) {
        log_write("tty: invalid grid dims", KERNEL, LOG_ERROR);
        return;
    }
    grid = (char*)kmalloc((usize)cols * (usize)rows);
    if (!grid) {
        log_write("tty: grid kmalloc failed", KERNEL, LOG_ERROR);
        return;
    }
    memset(grid, 0, (usize)cols * (usize)rows);
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
    u32 *new_fb = framebuffer_buffer();

    int new_cols = (new_px_w - 2 * TTY_PAD) / cell_width();
    int new_rows = (new_px_h - 2 * TTY_PAD) / cell_height();
    if (new_cols <= 0 || new_rows <= 0) {
        log_write("tty: invalid resized dims", KERNEL, LOG_ERROR);
        return;
    }

    px_w = new_px_w; px_h = new_px_h; stride = new_stride; fb = new_fb;

    if (reflow_grid(new_cols, new_rows) != 0) {
        log_write("tty: resize kmalloc failed", KERNEL, LOG_ERROR);
        return;
    }
    dirty = 1;
}

static int kernel_zoom(int delta) {
    if (!ready) return -1;
    if (delta == 0) return scale;

    int new_scale = scale;
    if (delta > 0 && new_scale < TTY_SCALE_MAX) new_scale++;
    if (delta < 0 && new_scale > TTY_SCALE_MIN) new_scale--;
    if (new_scale == scale) return scale;

    int new_cols = (px_w - 2 * TTY_PAD) / (FONT_GLYPH_W * new_scale);
    int new_rows = (px_h - 2 * TTY_PAD) / (FONT_GLYPH_H * new_scale);
    if (reflow_grid(new_cols, new_rows) != 0) {
        log_write("tty: zoom kmalloc failed", KERNEL, LOG_ERROR);
        return -1;
    }

    int old_scale = scale;
    scale = new_scale;
    dirty = 1;
    drain_push(scale > old_scale ? TTY_CTRL_ZOOM_IN : TTY_CTRL_ZOOM_OUT);
    if (active) render();
    return scale;
}

static void kernel_putc(char c) {
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
        do { kernel_putc(' '); } while (cx % 8);
        return;
    }
    if (cx >= cols) newline();
    grid[cy * cols + cx] = c;
    cx++;
    dirty = 1;
}

static void kernel_write(const char *buf, usize n) {
    if (!buf) return;
    for (usize i = 0; i < n; i++) kernel_putc(buf[i]);
}

static void kernel_clear(void) {
    if (!ready) return;
    memset(grid, 0, (usize)cols * (usize)rows);
    cx = cy = 0;
    dirty = 1;
    /* Also notify drain consumers (winman) so their mirror clears too. */
    drain_push(TTY_CTRL_CLEAR);
}

static int kernel_push(void) {
    if (!ready) return -1;
    usize bytes = (usize)cols * (usize)rows;
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

static int kernel_pop(void) {
    if (!ready || !saved_grid) return -1;
    usize bytes = (usize)cols * (usize)rows;
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

/* --- channel-addressed writes -----------------------------------------
 *
 * Channel 0 goes through the grid path above so the kernel keeps rendering
 * it. The others have no grid: their whole state is the drain ring, and the
 * control codes are exactly what tells winman to clear, save, restore or
 * rescale the console window mirroring them. */

void tty_write_ch(int idx, const char *buf, usize n) {
    if (!buf)
        return;
    if (idx == TTY_KERNEL) {
        kernel_write(buf, n);
        return;
    }
    if (!chan_valid(idx))
        return;
    for (usize i = 0; i < n; i++)
        chan_drain_push(&chans[idx], buf[i]);
}

void tty_clear_ch(int idx) {
    if (idx == TTY_KERNEL) {
        kernel_clear();
        return;
    }
    if (!chan_valid(idx))
        return;
    chan_drain_push(&chans[idx], TTY_CTRL_CLEAR);
}

int tty_push_ch(int idx) {
    if (idx == TTY_KERNEL)
        return kernel_push();
    if (!chan_valid(idx))
        return -1;
    chan_drain_push(&chans[idx], TTY_CTRL_PUSH);
    return 0;
}

int tty_pop_ch(int idx) {
    if (idx == TTY_KERNEL)
        return kernel_pop();
    if (!chan_valid(idx))
        return -1;
    chan_drain_push(&chans[idx], TTY_CTRL_POP);
    return 0;
}

/* No grid to reflow off-channel, so there is no scale to return: winman
 * owns the console's scale and clamps it. 0 means "code delivered". */
int tty_zoom_ch(int idx, int delta) {
    if (idx == TTY_KERNEL)
        return kernel_zoom(delta);
    if (!chan_valid(idx) || delta == 0)
        return -1;
    chan_drain_push(&chans[idx], delta > 0 ? TTY_CTRL_ZOOM_IN
                                           : TTY_CTRL_ZOOM_OUT);
    return 0;
}

/* Compositor kthread for the kernel TTY. Sleeps a few ticks between
 * frames , the kernel TTY is a fallback, not a game. Also tracks the
 * input owner so we automatically resume drawing when a winman
 * process dies and surrenders ownership.
 *
 * Doubles as the virtio-gpu pump: each iteration polls the device for a
 * host window-resize event, reflows the grid if one arrived, and pushes
 * the backbuffer to the scanout when we drew something. In MB2 mode both
 * calls degrade to no-ops, so the thread behaves identically there. */
void tty_thread_entry(void) {
    u32 seen_resize_generation = framebuffer_resize_generation();
    for (;;) {
        u32 resize_generation = framebuffer_resize_generation();
        if (resize_generation != seen_resize_generation) {
            seen_resize_generation = resize_generation;
            tty_resize();
        }

        int owner = msg_input_owner();
        int want_active = (owner == 0);
        if (want_active != active) tty_set_active(want_active);

        if (ready && active && dirty) render();
        /* Flush is owned by framebuffer_flush_thread_entry now , it polls
         * the damage rect at PIT tick rate independently of the tty render
         * cadence so a slow virtio ACK can't stall this thread. */

        task_sleep_ticks(3);
    }
}
