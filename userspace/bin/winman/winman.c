/*
 * winman — userspace window manager. Pure-userspace successor to the
 * old in-kernel WM. Owns the framebuffer, composites a desktop with
 * client-allocated window surfaces, and serves WM requests via IPC.
 *
 * Boots in parallel with the kernel-spawned shell: when winman is up
 * the kernel TTY is suppressed (we drain its ring instead) and the
 * desktop appears; when winman exits, the kernel TTY resumes and the
 * shell falls back to plain text mode.
 */

// #region INCLUDES

#include "../../lib/syscall.h"
#include "display/font8x8.h"
#include "utilities/types.h"
#include <stdint.h>
#include <stddef.h>

/* In-band TTY control codes — kept in sync with src/intf/display/tty.h.
 * Re-defined here (rather than including the kernel header) because
 * tty.h also declares `tty_drain` with a `size_t` signature that conflicts
 * with the userspace `long` wrapper in lib/syscall.h. */
#define TTY_CTRL_CLEAR  0x0C
#define TTY_CTRL_PUSH   0x1C
#define TTY_CTRL_POP    0x1D

// #endregion INCLUDES

// #region EXTERNS

extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void *memmove(void *, const void *, size_t);
extern size_t strlen(const char *);
extern int  strcmp(const char *, const char *);
extern void *malloc(size_t);
extern void  free(void *);
extern int   printf(const char *, ...);

/* Forward decls — defined in HELPERS region below, used by GEOMETRY + DRAG
 * helpers that have to come before HELPERS for declaration-order reasons. */
static void *aligned_page_alloc(size_t npages, void **out_raw);

// #endregion EXTERNS



// #region CURSOR SPRITE

#define CURSOR_W 12
#define CURSOR_H 12
#define COLOR_BORDER 0x00000000u
#define COLOR_FILL   0x00FFFFFFu

static const uint8_t cursor_mask[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,1,1,1,1,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

// #endregion CURSOR SPRITE

// #region PALETTE

#define DESKTOP_BG    0x00008080u   /* teal — Win3-ish                        */
#define CHROME_BG     0x00C0C0C0u
#define TITLEBAR_FG   0x000000A0u   /* navy title bar (focused)               */
#define TITLEBAR_BG   0x00808080u
#define CHROME_TEXT   0x00FFFFFFu
#define CONSOLE_FG    0x00FFFFFFu
#define CONSOLE_BG    0x00000000u

#define BORDER_PX     1
#define TITLEBAR_PX   16

/* Drag affordances. RESIZE_GRIP = size of the bottom-right square that acts
 * as the resize handle. MIN_CLIENT_* = floor below which we refuse to shrink
 * (smaller and the chrome geometry would invert). */
#define RESIZE_GRIP   12
#define MIN_CLIENT_W  64
#define MIN_CLIENT_H  40

/* Hit-test region codes returned by hit_test_at. */
#define HIT_NONE      0
#define HIT_TITLEBAR  1
#define HIT_GRIP      2
#define HIT_CLIENT    3

/* Special handle reserved for the built-in console. Real client window
 * handles start at 1 (next_handle is initialised to 1 in main). */
#define HANDLE_CONSOLE 0

// #endregion PALETTE

// #region FRAMEBUFFER + BACK BUFFER

static uint32_t *fb_hw;
static uint32_t *fb;            /* back buffer (page-aligned malloc) */
static void     *fb_raw;
static size_t    fb_bytes;
static int       fb_w, fb_h, fb_stride;
static int       desktop_dirty = 1;

// #endregion FRAMEBUFFER + BACK BUFFER

// #region WINDOWS

#define MAX_WINDOWS 8

struct window {
    int       in_use;
    int       handle;
    int       owner_pid;
    int       x, y;
    int       client_w, client_h;
    char      title[48];
    uint32_t *surface;           /* page-aligned, owned by winman          */
    void     *surface_raw;       /* original malloc ptr for free()         */
    int       n_pages;
    uint64_t  client_va;         /* va in owner_pml4 (0 if not shared)     */
};

struct drag_state {
    int active;
    int kind;            /* HIT_TITLEBAR (move) or HIT_GRIP (resize) */
    int handle;          /* HANDLE_CONSOLE or a window handle        */
    int grab_mx, grab_my;
    int orig_x, orig_y;
    int orig_cw, orig_ch;

    /* Ghost rect bookkeeping. While a drag is in flight we don't recompose
     * or reallocate anything; we just draw a 1px outline on fb_hw at the
     * proposed position/size and restore the underlying pixels from the
     * (frozen) back buffer on every frame. Commit happens once on MOUSE_UP. */
    int have_ghost;
    int last_gx, last_gy, last_gw, last_gh;
};
static struct drag_state drag;

static struct window windows[MAX_WINDOWS];
static int next_handle = 1;
static int focused_handle = 0;

// #endregion WINDOWS

// #region CONSOLE WINDOW

struct console {
    int       enabled;
    int       x, y;
    int       client_w, client_h;
    int       cols, rows;
    int       cx, cy;
    uint32_t *surface;
    void     *raw;
    char      title[48];
};

static struct console con;

/* Alt-screen backing buffer for console push/pop. Allocated once at
 * con_alloc time, same size as the live surface. saved_valid gates pop so
 * a spurious pop without a prior push is a no-op. */
static uint32_t *con_backing       = 0;
static void     *con_backing_raw   = 0;
static int       con_saved_cx      = 0;
static int       con_saved_cy      = 0;
static int       con_saved_valid   = 0;

/* Pre-expanded glyph row for the console (CONSOLE_FG/CONSOLE_BG are
 * constants): byte -> 8 pixels. Built once in con_alloc. 8 KiB BSS.
 * Per-row glyph render becomes a single memcpy(32 B) instead of 8 per-pixel
 * conditional writes — the dominant cost when winman drains the TTY ring. */
static uint32_t con_glyph_lut[256][FONT_GLYPH_W];

static void build_con_glyph_lut(void) {
    for (int b = 0; b < 256; b++) {
        for (int c = 0; c < FONT_GLYPH_W; c++) {
            con_glyph_lut[b][c] = ((b >> c) & 1) ? CONSOLE_FG : CONSOLE_BG;
        }
    }
}

/* Fill `n` 32-bit words at `dst` with `color`. Writes in 64-bit pairs when
 * the destination is 8-aligned, falls back to single stores at the tail.
 * Used for big solid rectangles and console scroll fills. */
static void fill_dwords(uint32_t *dst, size_t n, uint32_t color) {
    if (((uintptr_t)dst & 7) == 0 && n >= 2) {
        uint64_t v = ((uint64_t)color << 32) | color;
        uint64_t *p = (uint64_t*)dst;
        size_t pairs = n >> 1;
        for (size_t i = 0; i < pairs; i++) p[i] = v;
        dst += pairs * 2;
        n   &= 1;
    }
    while (n--) *dst++ = color;
}

// #endregion CONSOLE WINDOW

// #region GEOMETRY + DRAG

/* Polymorphic accessors over (real-window | console) keyed by handle. The
 * console is conceptually a window but lives in its own struct because
 * winman owns the glyph rendering, not an external client. */
static int win_get_rect(int handle, int *x, int *y, int *cw, int *ch) {
    if (handle == HANDLE_CONSOLE) {
        if (!con.enabled) return 0;
        *x = con.x; *y = con.y; *cw = con.client_w; *ch = con.client_h;
        return 1;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].handle == handle) {
            *x  = windows[i].x;        *y  = windows[i].y;
            *cw = windows[i].client_w; *ch = windows[i].client_h;
            return 1;
        }
    }
    return 0;
}

static void win_set_pos(int handle, int x, int y) {
    if (handle == HANDLE_CONSOLE) { con.x = x; con.y = y; return; }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].handle == handle) {
            windows[i].x = x; windows[i].y = y; return;
        }
    }
}

static int outer_w_dims(int cw) { return cw + 2 * BORDER_PX; }
static int outer_h_dims(int ch) { return ch + TITLEBAR_PX + BORDER_PX; }

/* Classify a screen-space hit by walking topmost-first. Real windows are
 * drawn after the console, so they sit above it in z-order; we iterate the
 * window array in reverse-drawing order and fall through to the console. */
static int hit_test_at(int mx, int my, int *out_handle) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (!windows[i].in_use) continue;
        int x  = windows[i].x, y = windows[i].y;
        int ow = outer_w_dims(windows[i].client_w);
        int oh = outer_h_dims(windows[i].client_h);
        if (mx < x || mx >= x + ow || my < y || my >= y + oh) continue;
        *out_handle = windows[i].handle;
        /* Grip beats titlebar when they overlap at the seam. */
        if (mx >= x + ow - RESIZE_GRIP && my >= y + oh - RESIZE_GRIP) return HIT_GRIP;
        if (my < y + TITLEBAR_PX) return HIT_TITLEBAR;
        return HIT_CLIENT;
    }
    if (con.enabled) {
        int x = con.x, y = con.y;
        int ow = outer_w_dims(con.client_w);
        int oh = outer_h_dims(con.client_h);
        if (mx >= x && mx < x + ow && my >= y && my < y + oh) {
            *out_handle = HANDLE_CONSOLE;
            if (mx >= x + ow - RESIZE_GRIP && my >= y + oh - RESIZE_GRIP) return HIT_GRIP;
            if (my < y + TITLEBAR_PX) return HIT_TITLEBAR;
            return HIT_CLIENT;
        }
    }
    return HIT_NONE;
}

static void clamp_to_desktop(int *x, int *y, int cw, int ch) {
    int ow = outer_w_dims(cw);
    (void)ch;
    /* Keep at least the title bar reachable for re-drag. */
    if (*x + ow < TITLEBAR_PX)   *x = TITLEBAR_PX - ow;
    if (*y < 0)                  *y = 0;
    if (*x > fb_w - TITLEBAR_PX) *x = fb_w - TITLEBAR_PX;
    if (*y > fb_h - TITLEBAR_PX) *y = fb_h - TITLEBAR_PX;
}

/* Console resize: reallocate surface + backing buffer at new dims and copy
 * the still-visible region of the old surface into the new one so existing
 * glyphs survive. Cursor is clamped to the new grid. */
static void console_resize(int new_cw, int new_ch) {
    if (!con.enabled) return;
    /* Snap to glyph grid so cells line up without trailing fractional row. */
    new_cw = (new_cw / FONT_GLYPH_W) * FONT_GLYPH_W;
    new_ch = (new_ch / FONT_GLYPH_H) * FONT_GLYPH_H;
    if (new_cw < MIN_CLIENT_W) new_cw = MIN_CLIENT_W;
    if (new_ch < MIN_CLIENT_H) new_ch = MIN_CLIENT_H;
    if (new_cw == con.client_w && new_ch == con.client_h) return;

    size_t pixel_bytes = (size_t)new_cw * (size_t)new_ch * 4;
    size_t pages = (pixel_bytes + 4095) / 4096;

    void     *new_raw = 0;
    uint32_t *new_surf = (uint32_t*)aligned_page_alloc(pages, &new_raw);
    if (!new_surf) return;
    fill_dwords(new_surf, (size_t)new_cw * (size_t)new_ch, CONSOLE_BG);

    void     *new_back_raw = 0;
    uint32_t *new_back = (uint32_t*)aligned_page_alloc(pages, &new_back_raw);
    if (!new_back) { free(new_raw); return; }

    /* Copy the upper-left intersection of old and new surfaces, row-by-row,
     * so the user keeps their scrollback visible while the window grows
     * or shrinks. Anything past the intersection stays at CONSOLE_BG. */
    if (con.surface) {
        int copy_w = con.client_w < new_cw ? con.client_w : new_cw;
        int copy_h = con.client_h < new_ch ? con.client_h : new_ch;
        for (int y = 0; y < copy_h; y++) {
            memcpy(new_surf   + (size_t)y * (size_t)new_cw,
                   con.surface + (size_t)y * (size_t)con.client_w,
                   (size_t)copy_w * 4);
        }
    }

    if (con.raw)         free(con.raw);
    if (con_backing_raw) free(con_backing_raw);

    con.surface      = new_surf;
    con.raw          = new_raw;
    con_backing      = new_back;
    con_backing_raw  = new_back_raw;
    con.client_w     = new_cw;
    con.client_h     = new_ch;
    con.cols         = new_cw / FONT_GLYPH_W;
    con.rows         = new_ch / FONT_GLYPH_H;
    if (con.cx >= con.cols) con.cx = con.cols - 1;
    if (con.cy >= con.rows) con.cy = con.rows - 1;
    con_saved_valid  = 0;
    desktop_dirty    = 1;
}

/* Client window resize: allocate a fresh surface, copy the old visible
 * region into it, re-share into the owner's address space, and notify the
 * client with the new geometry. Old surface is freed once the new one is
 * shared.
 *
 * Race window: the client still has its old client_va mapped until it
 * processes IPC_WM_RESIZE_NOTIFY. If it writes to the old va after we free
 * but before the notify is consumed, those writes land on freed phys pages
 * that may have been recycled. Clients are expected to stop drawing on
 * receipt of the notify; well-behaved ones won't race. */
static void client_window_resize(int handle, int new_cw, int new_ch) {
    struct window *w = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].handle == handle) { w = &windows[i]; break; }
    }
    if (!w) return;
    if (new_cw < MIN_CLIENT_W) new_cw = MIN_CLIENT_W;
    if (new_ch < MIN_CLIENT_H) new_ch = MIN_CLIENT_H;
    if (new_cw == w->client_w && new_ch == w->client_h) return;

    size_t pixel_bytes = (size_t)new_cw * (size_t)new_ch * 4;
    size_t pages = (pixel_bytes + 4095) / 4096;

    void *raw = 0;
    uint32_t *surface = (uint32_t*)aligned_page_alloc(pages, &raw);
    if (!surface) return;
    memset(surface, 0, pages * 4096);

    /* Preserve the upper-left intersection of the old surface — same idea
     * as console_resize. The client can't paint until it receives the
     * resize notify, so racing it for these reads is acceptable. */
    if (w->surface) {
        int copy_w = w->client_w < new_cw ? w->client_w : new_cw;
        int copy_h = w->client_h < new_ch ? w->client_h : new_ch;
        for (int y = 0; y < copy_h; y++) {
            memcpy(surface    + (size_t)y * (size_t)new_cw,
                   w->surface + (size_t)y * (size_t)w->client_w,
                   (size_t)copy_w * 4);
        }
    }

    uint64_t client_va = 0;
    if (shmem_share(w->owner_pid, (uint64_t)surface, (long)pages, &client_va) != 0) {
        free(raw);
        return;
    }

    void *old_raw = w->surface_raw;

    w->surface     = surface;
    w->surface_raw = raw;
    w->n_pages     = (int)pages;
    w->client_va   = client_va;
    w->client_w    = new_cw;
    w->client_h    = new_ch;
    desktop_dirty  = 1;

    struct ipc_msg note;
    memset(&note, 0, sizeof(note));
    note.type  = IPC_WM_RESIZE_NOTIFY;
    note.a     = handle;
    note.b     = new_cw;
    note.c     = new_ch;
    note.va    = client_va;
    note.pitch = (uint32_t)(new_cw * 4);
    ipc_send(w->owner_pid, &note);

    if (old_raw) free(old_raw);
}

// #endregion GEOMETRY + DRAG

// #region HELPERS

static void *aligned_page_alloc(size_t npages, void **out_raw) {
    size_t need = npages * 4096 + 4095;
    void *raw = malloc(need);
    if (!raw) return 0;
    memset(raw, 0, need);
    uintptr_t v = ((uintptr_t)raw + 4095) & ~(uintptr_t)4095;
    if (out_raw) *out_raw = raw;
    return (void*)v;
}

static inline uint32_t *fb_pix(int x, int y) {
    return fb + (size_t)y * (size_t)fb_stride + (size_t)x;
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    uint32_t *row = fb_pix(x, y);
    for (int yy = 0; yy < h; yy++) {
        fill_dwords(row, (size_t)w, color);
        row += fb_stride;
    }
}

static void draw_glyph_fb(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (c < FONT_FIRST || c > FONT_LAST) c = ' ';
    /* Single-shot rect clip instead of per-pixel bounds checks. Glyphs that
     * land fully off-screen short-circuit; glyphs that partly hang off the
     * edges trim once and then the inner loops run clean. */
    if (x + FONT_GLYPH_W <= 0 || x >= fb_w) return;
    if (y + FONT_GLYPH_H <= 0 || y >= fb_h) return;
    int col_first = x < 0 ? -x : 0;
    int col_last  = x + FONT_GLYPH_W > fb_w ? fb_w - x : FONT_GLYPH_W;
    int row_first = y < 0 ? -y : 0;
    int row_last  = y + FONT_GLYPH_H > fb_h ? fb_h - y : FONT_GLYPH_H;
    const uint8_t *glyph = font8x8[(int)c - FONT_FIRST];
    for (int r = row_first; r < row_last; r++) {
        uint8_t bits = glyph[r];
        /* Address by row-start + absolute x. Avoids feeding a negative x to
         * fb_pix, where (size_t) cast would wrap and point miles off-screen. */
        uint32_t *row = fb_pix(0, y + r);
        for (int col = col_first; col < col_last; col++) {
            row[x + col] = (bits & (1 << col)) ? fg : bg;
        }
    }
}

static void draw_text_fb(int x, int y, const char *s, int max_w,
                         uint32_t fg, uint32_t bg) {
    int drawn = 0;
    while (*s && drawn + FONT_GLYPH_W <= max_w) {
        draw_glyph_fb(x + drawn, y, *s, fg, bg);
        s++;
        drawn += FONT_GLYPH_W;
    }
}

static int outer_w(const struct window *w) { return w->client_w + 2 * BORDER_PX; }
static int outer_h(const struct window *w) { return w->client_h + TITLEBAR_PX + BORDER_PX; }

static void draw_chrome(const struct window *w, int focused) {
    int ow = outer_w(w);
    int oh = outer_h(w);
    fb_fill_rect(w->x, w->y, ow, oh, CHROME_BG);
    int tb_x  = w->x + BORDER_PX;
    int tb_y  = w->y;
    int tb_w  = ow - 2 * BORDER_PX;
    int tb_h  = TITLEBAR_PX - BORDER_PX;
    uint32_t bg = focused ? TITLEBAR_FG : TITLEBAR_BG;
    fb_fill_rect(tb_x, tb_y, tb_w, tb_h, bg);
    int text_x = tb_x + 4;
    int text_y = tb_y + (tb_h - FONT_GLYPH_H) / 2;
    int avail  = tb_w - 8;
    if (avail > 0) {
        draw_text_fb(text_x, text_y, w->title, avail, CHROME_TEXT, bg);
    }
}

static void blit_surface(const struct window *w) {
    int dst_x = w->x + BORDER_PX;
    int dst_y = w->y + TITLEBAR_PX;
    int cw    = w->client_w;
    int ch    = w->client_h;

    int x0 = dst_x < 0 ? 0 : dst_x;
    int y0 = dst_y < 0 ? 0 : dst_y;
    int x1 = (dst_x + cw) > fb_w ? fb_w : dst_x + cw;
    int y1 = (dst_y + ch) > fb_h ? fb_h : dst_y + ch;
    if (x0 >= x1 || y0 >= y1) return;

    int src_x = x0 - dst_x;
    int src_y = y0 - dst_y;
    int span_w = x1 - x0;

    for (int yy = 0; yy < y1 - y0; yy++) {
        uint32_t *src = w->surface + (size_t)(src_y + yy) * (size_t)cw + (size_t)src_x;
        uint32_t *dst = fb_pix(x0, y0 + yy);
        memcpy(dst, src, (size_t)span_w * 4);
    }
}

// #endregion HELPERS

// #region CONSOLE (TTY DRAIN)

static void con_draw_glyph(int gx, int gy, char c) {
    int px = gx * FONT_GLYPH_W;
    int py = gy * FONT_GLYPH_H;
    if (px + FONT_GLYPH_W > con.client_w) return;
    if (py + FONT_GLYPH_H > con.client_h) return;
    const uint8_t *glyph;
    static const uint8_t blank[FONT_GLYPH_H] = {0};
    if (c < FONT_FIRST || c > FONT_LAST) glyph = blank;
    else                                 glyph = font8x8[(int)c - FONT_FIRST];
    uint32_t *line = con.surface + (size_t)py * (size_t)con.client_w + (size_t)px;
    for (int r = 0; r < FONT_GLYPH_H; r++) {
        memcpy(line, con_glyph_lut[glyph[r]], FONT_GLYPH_W * sizeof(uint32_t));
        line += con.client_w;
    }
}

static void con_scroll(void) {
    size_t row_pixels = (size_t)con.client_w;
    int    shift      = FONT_GLYPH_H;
    int    copy_rows  = con.client_h - shift;
    if (copy_rows > 0) {
        memmove(con.surface,
                con.surface + (size_t)shift * row_pixels,
                (size_t)copy_rows * row_pixels * 4);
    }
    int blank_first = copy_rows > 0 ? copy_rows : 0;
    uint32_t *line = con.surface + (size_t)blank_first * row_pixels;
    for (int y = blank_first; y < con.client_h; y++) {
        fill_dwords(line, row_pixels, CONSOLE_BG);
        line += row_pixels;
    }
}

static void con_newline(void) {
    con.cx = 0;
    con.cy++;
    if (con.cy >= con.rows) { con_scroll(); con.cy = con.rows - 1; }
}

/* Wipe the live console surface back to CONSOLE_BG and home the cursor.
 * Used by both TTY_CTRL_CLEAR and the entry path of TTY_CTRL_PUSH. */
static void con_wipe(void) {
    if (!con.enabled || !con.surface) return;
    fill_dwords(con.surface,
                (size_t)con.client_w * (size_t)con.client_h,
                CONSOLE_BG);
    con.cx = con.cy = 0;
    desktop_dirty = 1;
}

static void con_save(void) {
    if (!con.enabled || !con.surface || !con_backing) return;
    size_t pixels = (size_t)con.client_w * (size_t)con.client_h;
    memcpy(con_backing, con.surface, pixels * 4);
    con_saved_cx = con.cx;
    con_saved_cy = con.cy;
    con_saved_valid = 1;
}

static void con_restore(void) {
    if (!con.enabled || !con.surface || !con_backing || !con_saved_valid) return;
    size_t pixels = (size_t)con.client_w * (size_t)con.client_h;
    memcpy(con.surface, con_backing, pixels * 4);
    con.cx = con_saved_cx;
    con.cy = con_saved_cy;
    con_saved_valid = 0;
    desktop_dirty = 1;
}

static void con_putc(char c) {
    if (!con.enabled) return;
    desktop_dirty = 1;
    if (c == '\n') { con_newline(); return; }
    if (c == '\r') { con.cx = 0; return; }
    if (c == '\b') {
        if (con.cx > 0) { con.cx--; con_draw_glyph(con.cx, con.cy, ' '); }
        return;
    }
    if (c == '\t') {
        do { con_putc(' '); } while (con.cx % 8);
        return;
    }
    if (c == TTY_CTRL_CLEAR) { con_wipe(); return; }
    if (c == TTY_CTRL_PUSH)  { con_save(); con_wipe(); return; }
    if (c == TTY_CTRL_POP)   { con_restore(); return; }
    if (con.cx >= con.cols) con_newline();
    con_draw_glyph(con.cx, con.cy, c);
    con.cx++;
}

static void con_alloc(void) {
    int margin = 16;
    int cw = fb_w - 2 * margin - 2 * BORDER_PX;
    int ch = fb_h - 2 * margin - TITLEBAR_PX - BORDER_PX;
    if (cw < 64 || ch < 64) return;
    cw = (cw / FONT_GLYPH_W) * FONT_GLYPH_W;
    ch = (ch / FONT_GLYPH_H) * FONT_GLYPH_H;

    size_t pixel_bytes = (size_t)cw * (size_t)ch * 4;
    size_t pages = (pixel_bytes + 4095) / 4096;
    con.surface = (uint32_t*)aligned_page_alloc(pages, &con.raw);
    if (!con.surface) return;

    fill_dwords(con.surface, (size_t)cw * (size_t)ch, CONSOLE_BG);

    /* Alt-screen backing buffer — same dimensions as the live surface so
     * memcpy push/pop is unconditional. Allocated once; never freed. */
    con_backing = (uint32_t*)aligned_page_alloc(pages, &con_backing_raw);

    build_con_glyph_lut();
    con.client_w = cw;
    con.client_h = ch;
    con.cols     = cw / FONT_GLYPH_W;
    con.rows     = ch / FONT_GLYPH_H;
    con.x = margin;
    con.y = margin;
    con.cx = con.cy = 0;
    const char *t = "Console";
    size_t tn = 0;
    while (t[tn] && tn < sizeof(con.title) - 1) { con.title[tn] = t[tn]; tn++; }
    con.title[tn] = 0;
    con.enabled = 1;
}

static void drain_tty_into_console(void) {
    char buf[256];
    long n;
    while ((n = tty_drain(buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) con_putc(buf[i]);
        if (n < (long)sizeof(buf)) break;
    }
}

// #endregion CONSOLE (TTY DRAIN)

// #region COMPOSITOR

static void compose(void) {
    fb_fill_rect(0, 0, fb_w, fb_h, DESKTOP_BG);

    if (con.enabled) {
        struct window cw = {0};
        cw.x = con.x; cw.y = con.y;
        cw.client_w = con.client_w; cw.client_h = con.client_h;
        cw.surface = con.surface;
        memcpy(cw.title, con.title, sizeof(cw.title) - 1);
        draw_chrome(&cw, focused_handle == 0);
        blit_surface(&cw);
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        int focused = windows[i].handle == focused_handle;
        draw_chrome(&windows[i], focused);
        blit_surface(&windows[i]);
    }
}

static int cursor_scale(void) {
    int s = (fb_w > 0 ? fb_w : 720) / 1080;
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    return s;
}

static void draw_cursor(int32_t x, int32_t y) {
    int scale = cursor_scale();
    int draw_w = CURSOR_W * scale;
    int draw_h = CURSOR_H * scale;

    int x0 = x < 0 ? 0 : (int)x;
    int y0 = y < 0 ? 0 : (int)y;
    int x1 = ((int)x + draw_w) > fb_w ? fb_w : (int)x + draw_w;
    int y1 = ((int)y + draw_h) > fb_h ? fb_h : (int)y + draw_h;

    if (x0 >= x1 || y0 >= y1) return;

    for (int yy = 0; yy < y1 - y0; yy++) {
        uint32_t *p = fb_hw
            + (size_t)(y0 + yy) * (size_t)fb_stride
            + (size_t)x0;

        int src_y = (y0 + yy - (int)y) / scale;

        for (int xx = 0; xx < x1 - x0; xx++) {
            int src_x = (x0 + xx - (int)x) / scale;
            uint8_t mv = cursor_mask[src_y][src_x];

            if (mv == 1)      p[xx] = COLOR_BORDER;
            else if (mv == 2) p[xx] = COLOR_FILL;
        }
    }
    fb_damage((uint32_t)x0, (uint32_t)y0,
              (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
}

static void present_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *src = fb_pix(x, y + yy);
        uint32_t *dst = fb_hw + (size_t)(y + yy) * (size_t)fb_stride + (size_t)x;
        memcpy(dst, src, (size_t)w * 4);
    }
    fb_damage((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

/* Restore the four 1px sides of a previously-drawn ghost outline by blitting
 * the corresponding strips from the (frozen-during-drag) back buffer. */
static void erase_ghost(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    present_rect(x,         y,         w, 1);   /* top    */
    present_rect(x,         y + h - 1, w, 1);   /* bottom */
    present_rect(x,         y,         1, h);   /* left   */
    present_rect(x + w - 1, y,         1, h);   /* right  */
}

/* Draw a 1px white outline directly on fb_hw at the proposed drag position.
 * Bypasses the back buffer so the underlying composite stays untouched and
 * erase_ghost can restore from it next frame. */
static void draw_ghost(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    uint32_t c = 0x00FFFFFFu;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x0 >= x1 || y0 >= y1) return;

    if (y >= 0 && y < fb_h) {
        uint32_t *row = fb_hw + (size_t)y * (size_t)fb_stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
    int yb = y + h - 1;
    if (yb >= 0 && yb < fb_h) {
        uint32_t *row = fb_hw + (size_t)yb * (size_t)fb_stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
    if (x >= 0 && x < fb_w) {
        for (int yy = y0; yy < y1; yy++) {
            fb_hw[(size_t)yy * (size_t)fb_stride + x] = c;
        }
    }
    int xr = x + w - 1;
    if (xr >= 0 && xr < fb_w) {
        for (int yy = y0; yy < y1; yy++) {
            fb_hw[(size_t)yy * (size_t)fb_stride + xr] = c;
        }
    }
    /* Damage covers the bbox of the outline (slight overdraw — the interior
     * of the rect wasn't touched, but bbox tracking inside the kernel
     * coalesces anyway). */
    fb_damage((uint32_t)x0, (uint32_t)y0,
              (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
}

/* Compute the outer rect of the proposed drag target. For move drags the
 * size is fixed at the original; for resize drags the position is fixed and
 * the size grows/shrinks with the mouse delta. */
static void compute_ghost(int mx, int my, int *gx, int *gy, int *gw, int *gh) {
    int dx = mx - drag.grab_mx;
    int dy = my - drag.grab_my;
    if (drag.kind == HIT_TITLEBAR) {
        int nx = drag.orig_x + dx;
        int ny = drag.orig_y + dy;
        clamp_to_desktop(&nx, &ny, drag.orig_cw, drag.orig_ch);
        *gx = nx; *gy = ny;
        *gw = outer_w_dims(drag.orig_cw);
        *gh = outer_h_dims(drag.orig_ch);
    } else {
        int ncw = drag.orig_cw + dx;
        int nch = drag.orig_ch + dy;
        if (ncw < MIN_CLIENT_W) ncw = MIN_CLIENT_W;
        if (nch < MIN_CLIENT_H) nch = MIN_CLIENT_H;
        *gx = drag.orig_x;
        *gy = drag.orig_y;
        *gw = outer_w_dims(ncw);
        *gh = outer_h_dims(nch);
    }
}

// #endregion COMPOSITOR

// #region WINDOW REGISTRY

static struct window *find_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].in_use) return &windows[i];
    }
    return 0;
}

static struct window *find_handle(int handle) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].handle == handle) return &windows[i];
    }
    return 0;
}

static int handle_create(int client_pid, int w, int h, const char *title,
                         uint64_t *out_client_va, uint32_t *out_pitch,
                         int *out_handle) {
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return -1;
    struct window *win = find_slot();
    if (!win) return -1;

    size_t pixel_bytes = (size_t)w * (size_t)h * 4;
    size_t pages = (pixel_bytes + 4095) / 4096;

    void *raw = 0;
    uint32_t *surface = (uint32_t*)aligned_page_alloc(pages, &raw);
    if (!surface) return -1;
    memset(surface, 0, pages * 4096);

    /* Map the same physical pages into the client's PML4 so the client
     * can write pixels without going through winman. */
    uint64_t client_va = 0;
    if (shmem_share(client_pid, (uint64_t)surface, (long)pages, &client_va) != 0) {
        free(raw);
        return -1;
    }

    win->in_use   = 1;
    win->handle   = next_handle++;
    win->owner_pid = client_pid;
    /* Cascade placement: stagger new windows down-right so they don't
     * all stack at (0,0). */
    int placed = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (&windows[i] != win && windows[i].in_use) placed++;
    }
    win->x = 60 + placed * 24;
    win->y = 60 + placed * 24;
    win->client_w   = w;
    win->client_h   = h;
    win->surface    = surface;
    win->surface_raw = raw;
    win->n_pages    = (int)pages;
    win->client_va  = client_va;
    if (title) {
        size_t i = 0;
        while (i < sizeof(win->title) - 1 && title[i]) {
            win->title[i] = title[i];
            i++;
        }
        win->title[i] = 0;
    } else {
        win->title[0] = 0;
    }

    focused_handle = win->handle;
    desktop_dirty = 1;

    *out_client_va = client_va;
    *out_pitch     = (uint32_t)(w * 4);
    *out_handle    = win->handle;
    return 0;
}

static void handle_destroy(int client_pid, int handle) {
    struct window *w = find_handle(handle);
    if (!w || w->owner_pid != client_pid) return;
    if (w->surface_raw) free(w->surface_raw);
    memset(w, 0, sizeof(*w));
    if (focused_handle == handle) focused_handle = 0;
    desktop_dirty = 1;
}

static void handle_set_title(int handle, const char *title) {
    struct window *w = find_handle(handle);
    if (!w || !title) return;
    size_t i = 0;
    while (i < sizeof(w->title) - 1 && title[i]) {
        w->title[i] = title[i];
        i++;
    }
    w->title[i] = 0;
    desktop_dirty = 1;
}

// #endregion WINDOW REGISTRY

// #region IPC PUMP

static void send_create_resp(int target_pid, int handle, uint64_t va,
                             uint32_t pitch, int w, int h) {
    struct ipc_msg resp;
    memset(&resp, 0, sizeof(resp));
    resp.type  = IPC_WM_CREATE_RESP;
    resp.a     = handle;
    resp.b     = w;
    resp.c     = h;
    resp.va    = va;
    resp.pitch = pitch;
    ipc_send(target_pid, &resp);
}

static void pump_ipc(void) {
    struct ipc_msg m;
    while (ipc_recv(&m)) {
        int from = (int)m.from_pid;
        switch (m.type) {
        case IPC_WM_CREATE_REQ: {
            uint64_t va = 0;
            uint32_t pitch = 0;
            int      handle = 0;
            int rc = handle_create(from, m.a, m.b, m.str, &va, &pitch, &handle);
            if (rc == 0) {
                send_create_resp(from, handle, va, pitch, m.a, m.b);
            } else {
                send_create_resp(from, -1, 0, 0, m.a, m.b);
            }
            break;
        }
        case IPC_WM_DESTROY_REQ:
            handle_destroy(from, m.a);
            break;
        case IPC_WM_INVALIDATE_REQ:
            desktop_dirty = 1;
            break;
        case IPC_WM_SET_TITLE_REQ:
            handle_set_title(m.a, m.str);
            break;
        default:
            break;
        }
    }
}

// #endregion IPC PUMP

// #region INPUT PUMP

static void forward_input(int target_pid, const struct msg *m) {
    if (target_pid <= 0) return;
    struct ipc_msg out;
    memset(&out, 0, sizeof(out));
    out.type = IPC_WM_INPUT;
    out.a    = m->type;
    out.b    = m->param;
    out.c    = m->x;
    out.d    = m->y;
    ipc_send(target_pid, &out);
}

static void pump_input(void) {
    struct msg m;
    while (msg_get(&m)) {
        int forward = 1;

        /* In-flight drag: suppress forwarding so the focused client doesn't
         * see phantom cursor scribbles across its window. MOUSE_MOVE is a
         * no-op here — the main loop polls mouse_pos and renders the ghost
         * outline directly. We only act on MOUSE_UP, where we commit the
         * final position or call the (expensive) resize once. */
        if (drag.active) {
            if (m.type == MSG_MOUSE_MOVE) {
                forward = 0;
            } else if (m.type == MSG_MOUSE_UP) {
                int dx = m.x - drag.grab_mx;
                int dy = m.y - drag.grab_my;
                if (drag.kind == HIT_TITLEBAR) {
                    int nx = drag.orig_x + dx;
                    int ny = drag.orig_y + dy;
                    clamp_to_desktop(&nx, &ny, drag.orig_cw, drag.orig_ch);
                    win_set_pos(drag.handle, nx, ny);
                } else if (drag.kind == HIT_GRIP) {
                    int new_cw = drag.orig_cw + dx;
                    int new_ch = drag.orig_ch + dy;
                    if (drag.handle == HANDLE_CONSOLE) {
                        console_resize(new_cw, new_ch);
                    } else {
                        client_window_resize(drag.handle, new_cw, new_ch);
                    }
                }
                drag.active   = 0;
                desktop_dirty = 1;
                forward = 0;
            } else if (m.type == MSG_MOUSE_DOWN) {
                /* Spurious extra down during drag — ignore. */
                forward = 0;
            }
        } else if (m.type == MSG_MOUSE_DOWN) {
            int hit_handle = 0;
            int kind = hit_test_at(m.x, m.y, &hit_handle);
            if (kind == HIT_TITLEBAR || kind == HIT_GRIP) {
                int x, y, cw, ch;
                if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
                    focused_handle = hit_handle;
                    drag.active    = 1;
                    drag.kind      = kind;
                    drag.handle    = hit_handle;
                    drag.grab_mx   = m.x;
                    drag.grab_my   = m.y;
                    drag.orig_x    = x;  drag.orig_y  = y;
                    drag.orig_cw   = cw; drag.orig_ch = ch;
                    desktop_dirty = 1;
                }
                forward = 0;
            } else if (kind == HIT_CLIENT) {
                focused_handle = hit_handle;
                desktop_dirty = 1;
                /* Console clicks don't get forwarded — no client owns it. */
                if (hit_handle == HANDLE_CONSOLE) forward = 0;
            } else {
                focused_handle = 0;
                desktop_dirty = 1;
                forward = 0;
            }
        }

        if (!forward) continue;
        if (focused_handle == HANDLE_CONSOLE) continue;
        struct window *focus = focused_handle ? find_handle(focused_handle) : 0;
        if (focus) forward_input(focus->owner_pid, &m);
    }
}

// #endregion INPUT PUMP

// #region MAIN

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("winman: start\n");
    if (wm_register() != 0) {
        printf("winman: wm_register failed\n");
        return 1;
    }
    printf("winman: registered\n");

    struct fb_info info;
    if (fb_info(&info) != 0) return 2;
    fb_hw  = (uint32_t*)fb_map();
    if (!fb_hw) return 3;

    fb_w     = (int)info.width;
    fb_h     = (int)info.height;
    fb_stride = (int)(info.pitch / 4);
    fb_bytes = info.pitch * info.height;
    printf("winman: fb %dx%d pitch=%d bytes=%d\n",
           fb_w, fb_h, fb_stride*4, (int)fb_bytes);

    size_t back_pages = (fb_bytes + 4095) / 4096;
    fb = (uint32_t*)aligned_page_alloc(back_pages, &fb_raw);
    if (!fb) { printf("winman: back buffer alloc failed\n"); return 4; }
    printf("winman: back buffer @%p pages=%d\n", (void*)fb, (int)back_pages);

    memset(windows, 0, sizeof(windows));
    next_handle = 1;
    focused_handle = 0;
    con_alloc();
    printf("winman: con.enabled=%d surface=%p w=%d h=%d\n",
           con.enabled, (void*)con.surface, con.client_w, con.client_h);

    int self_pid = (int)get_pid();
    int tick = 0;
    int32_t last_cx = 0, last_cy = 0;
    int     have_last = 0;

    int loops_logged = 0;
    for (;;) {
        if ((int)wm_pid() != self_pid) {
            have_last = 0;
            sleep_ticks(1);
            continue;
        }

        /* Host-driven resize: kernel may have re-pointed the scanout at a
         * different-sized backing under us. Re-query dims; on change, rebind
         * USER_FB_BASE (sys_fb_map walks the new scatter-gather page list)
         * and grow/shrink our back buffer to match. */
        {
            struct fb_info cur;
            if (fb_info(&cur) == 0 &&
                ((int)cur.width != fb_w || (int)cur.height != fb_h)) {
                fb_hw = (uint32_t*)fb_map();
                if (fb_hw) {
                    fb_w      = (int)cur.width;
                    fb_h      = (int)cur.height;
                    fb_stride = (int)(cur.pitch / 4);
                    fb_bytes  = cur.pitch * cur.height;

                    if (fb_raw) free(fb_raw);
                    size_t back_pages = (fb_bytes + 4095) / 4096;
                    fb = (uint32_t*)aligned_page_alloc(back_pages, &fb_raw);
                    if (!fb) {
                        printf("winman: back buffer realloc failed\n");
                        return 5;
                    }
                    desktop_dirty = 1;
                    have_last = 0;
                    printf("winman: rebound fb to %dx%d\n", fb_w, fb_h);
                }
            }
        }

        pump_ipc();
        pump_input();
        drain_tty_into_console();

        int32_t mx, my;
        uint8_t btns;
        mouse_pos(&mx, &my, &btns);
        (void)btns;

        /* Drag-in-flight fast path: skip compose entirely and just maintain
         * the ghost outline + cursor on top of the frozen last-composed
         * frame. fb (back buffer) is untouched so the strips we lift from
         * it during erase_ghost are still the right pixels. */
        if (drag.active) {
            if (drag.have_ghost) {
                erase_ghost(drag.last_gx, drag.last_gy,
                            drag.last_gw, drag.last_gh);
            }
            if (have_last) {
	            int s = cursor_scale();
	            present_rect(last_cx, last_cy, CURSOR_W * s, CURSOR_H * s);
            }
            int gx, gy, gw, gh;
            compute_ghost((int)mx, (int)my, &gx, &gy, &gw, &gh);
            draw_ghost(gx, gy, gw, gh);
            drag.last_gx = gx; drag.last_gy = gy;
            drag.last_gw = gw; drag.last_gh = gh;
            drag.have_ghost = 1;

            draw_cursor(mx, my);
            last_cx = mx; last_cy = my; have_last = 1;
            yield();
            continue;
        }

        /* Drag just ended this tick — wipe the lingering ghost outline
         * before the normal compose path runs. desktop_dirty was set by
         * MOUSE_UP so the next branch will repaint everything anyway. */
        if (drag.have_ghost) {
            erase_ghost(drag.last_gx, drag.last_gy,
                        drag.last_gw, drag.last_gh);
            drag.have_ghost = 0;
        }

        int recompose = desktop_dirty && (++tick % 2 == 0);
        if (loops_logged < 5) {
            printf("winman: loop dirty=%d tick=%d recomp=%d\n",
                   desktop_dirty, tick, recompose);
            loops_logged++;
        }
        if (recompose) {
            compose();
            if (loops_logged <= 7) { printf("winman: composed\n"); loops_logged++; }
            memcpy(fb_hw, fb, fb_bytes);
            fb_damage(0, 0, (uint32_t)fb_w, (uint32_t)fb_h);
            if (loops_logged <= 8) { printf("winman: presented\n"); loops_logged++; }
            desktop_dirty = 0;
            have_last = 0;        /* cursor was clobbered by present */
        } else if (have_last) {
            /* erase cursor by restoring back buffer over its old rect */
            int s = cursor_scale();
            present_rect(last_cx, last_cy, CURSOR_W * s, CURSOR_H * s);
        }

        draw_cursor(mx, my);
        last_cx = mx;
        last_cy = my;
        have_last = 1;

        if (loops_logged < 12) { printf("winman: pre-yield\n"); loops_logged++; }
        yield();
        if (loops_logged < 13) { printf("winman: post-yield\n"); loops_logged++; }
    }
    return 0;
}

// #endregion MAIN
