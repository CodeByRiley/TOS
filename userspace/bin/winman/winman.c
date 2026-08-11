/* userspace/bin/winman/winman.c — userspace window manager.
 *
 * Pure-userspace successor to the old in-kernel WM. Owns the framebuffer,
 * composites a desktop with client-allocated window surfaces, and serves
 * WM requests via the IPC protocol declared in lib/wm.h.
 *
 * Lifecycle:
 *   - Boots in parallel with the kernel-spawned shell.
 *   - While winman is up, the kernel TTY is suppressed; winman drains the
 *     TTY ring into its own built-in console window.
 *   - On exit, the kernel TTY resumes and the shell falls back to plain
 *     text mode.
 *
 * Section roadmap (in order of appearance below):
 *   - CURSOR SPRITE / PALETTE       — visual constants + bitmap masks
 *   - FRAMEBUFFER + BACK BUFFER     — display state
 *   - WINDOWS                       — slot pool, z-order
 *   - CONSOLE WINDOW                — the built-in TTY window
 *   - GEOMETRY                      — hit-testing + rect math
 *   - DRAG                          — move + resize ghost rect state
 *   - RENDERERS                     — draw_* helpers
 *   - COMPOSITOR                    — back-buffer recomposition
 *   - INPUT                         — kernel msg ring → events
 *   - IPC PUMP                      — wire protocol handler
 *   - MAIN                          — entry point + main loop
 */

#include "../../lib/syscall.h"
#include "../../lib/wm.h"
#include "display/font8x8.h"
#include "utilities/types.h"
#include <stdint.h>
#include <stddef.h>

/* In-band TTY control codes — must match kernel/display/tty.h. Defined
 * inline rather than #include'd because that header also declares
 * tty_drain() with a `size_t` signature that conflicts with the userspace
 * `long` wrapper in lib/syscall.h. */
#define TTY_CTRL_CLEAR    0x0C
#define TTY_CTRL_PUSH     0x1C
#define TTY_CTRL_POP      0x1D
#define TTY_CTRL_ZOOM_IN  0x1E
#define TTY_CTRL_ZOOM_OUT 0x1F

extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void *memmove(void *, const void *, size_t);
extern size_t strlen(const char *);
extern int  strcmp(const char *, const char *);
extern void *malloc(size_t);
extern void  free(void *);
extern int   printf(const char *, ...);

/* Forward decls — used by helpers that appear before their definitions
 * because GEOMETRY + DRAG sit ahead of the helper bag. */
struct window;
static void *aligned_page_alloc(size_t npages, void **out_raw);
static int   window_count(void);
static int   is_minimized(int handle);
static struct window *find_handle(int handle);
static void  con_redraw(void);
static void  titlebar_btn_rect(int win_x, int win_y, int outer_w,
                               int idx_from_right,
                               int *bx, int *by, int *bw, int *bh);



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

/* Taskbar: always-on-top strip pinned to the bottom of the desktop.
 * Buttons list the console + every client window; clicking a button focuses
 * (and would raise, if winman had explicit z-order) that handle. */
#define TASKBAR_PX            24
#define TASKBAR_BG            0x00808080u
#define TASKBAR_BTN_BG        0x00C0C0C0u
#define TASKBAR_BTN_BG_FOCUS  0x000000A0u
#define TASKBAR_BTN_TEXT      0x00000000u
#define TASKBAR_BTN_TEXT_FOC  0x00FFFFFFu
#define TASKBAR_BTN_W         120
#define TASKBAR_BTN_GAP       2
#define TASKBAR_PAD_Y         2

/* Drag affordances. RESIZE_GRIP = size of the bottom-right square that acts
 * as the resize handle. MIN_CLIENT_* = floor below which we refuse to shrink
 * (smaller and the chrome geometry would invert). */
#define RESIZE_GRIP   12
#define MIN_CLIENT_W  64
#define MIN_CLIENT_H  40
#define CON_SCALE_MIN 1
#define CON_SCALE_MAX 4

/* Hit-test region codes returned by hit_test_at. */
#define HIT_NONE       0
#define HIT_TITLEBAR   1
#define HIT_GRIP       2
#define HIT_CLIENT     3
#define HIT_BTN_CLOSE  4
#define HIT_BTN_MAX    5
#define HIT_BTN_MIN    6

/* Titlebar buttons: small square icons right-anchored in the titlebar.
 * Today only the close (X) button exists; min/max can slot in by bumping
 * `idx_from_right` in titlebar_btn_rect. Buttons render as bitmap masks
 * (same scheme as the cursor) so future icon swaps don't need new helpers. */
#define TB_BTN_SIZE        12
#define TB_BTN_GAP         2
#define TB_BTN_PAD_R       2
#define TB_BTN_BG          0x00C0C0C0u
#define TB_BTN_BG_HOVER    0x00FF6060u
#define TB_BTN_FG          0x00000000u

/* title-bar button glyphs. 0 = background (titlebar btn bg), 1 = foreground.
 * Drawn through draw_button_mask which fills the rect with `bg` first then
 * stamps `fg` only where the mask is 1. */
static const uint8_t btn_close_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,1,0,0,0,0,1,1,0,0},
    {0,0,1,1,1,0,0,1,1,1,0,0},
    {0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,1,1,1,0,0,1,1,1,0,0},
    {0,0,1,1,0,0,0,0,1,1,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

static const uint8_t btn_maximise_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,1,0,0,0,0,1,1,0,0},
    {0,0,1,1,0,0,0,0,1,1,0,0},
    {0,0,1,1,0,0,0,0,1,1,0,0},
    {0,0,1,1,0,0,0,0,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

static const uint8_t btn_hide_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

/* Special handle reserved for the built-in console. Real client window
 * handles are derived from their slot index: handle = (slot_index + 1),
 * so they always live in 1..MAX_WINDOWS and get reused as soon as the
 * slot is freed. No monotonically-growing counter. */
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
    uint32_t *surface;       /* page-aligned, owned by winman          */
    void     *surface_raw;   /* original malloc ptr for free()         */
    uint64_t  client_va;     /* va in owner_pml4 (0 if not shared)     */

    int       in_use;
    int       handle;
    int       owner_pid;

    int       x, y;
    int       client_w, client_h;

    int       n_pages;

    /* Title-bar button state. minimized: skip compose + hit-test, but stay
     * on taskbar; clicking the taskbar button restores. maximized: window
     * fills the desktop (above taskbar); pre-max geometry is saved here so
     * a second click restores. */
    int       minimized;
    int       maximized;
    int       saved_x, saved_y;
    int       saved_cw, saved_ch;

    char      title[48];
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
static int focused_handle = 0;

/* Z-order: handles ordered front-to-back. z_order[0] is topmost (drawn
 * last, hit-tested first). Console takes a slot too — it can be raised
 * over client windows just like any other surface. Re-bound on every
 * create / focus / destroy so the array always reflects current stacking. */
#define MAX_Z (1 + MAX_WINDOWS)
static int z_order[MAX_Z];
static int z_count = 0;

static void z_remove(int handle) {
    int j = 0;
    for (int i = 0; i < z_count; i++) {
        if (z_order[i] == handle) continue;
        z_order[j++] = z_order[i];
    }
    z_count = j;
}

static void z_bring_to_front(int handle) {
    z_remove(handle);
    if (z_count >= MAX_Z) return;
    for (int i = z_count; i > 0; i--) z_order[i] = z_order[i - 1];
    z_order[0]  = handle;
    z_count++;
}


// #endregion WINDOWS

// #region CONSOLE WINDOW

struct console {
    uint32_t *surface;
    void     *raw;
    char     *cells;

    int       enabled;
    int       x, y;
    int       client_w, client_h;
    int       cols, rows;
    int       cx, cy;
    int       scale;

    char      title[48];
};

static struct console con;

/* Alt-screen backing buffer for console push/pop. Allocated once at
 * con_alloc time, same size as the live surface. saved_valid gates pop so
 * a spurious pop without a prior push is a no-op. */
static uint32_t *con_backing       = 0;
static void     *con_backing_raw   = 0;
static char     *con_saved_cells   = 0;
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

/* True iff (mx,my) lies inside titlebar button `idx_from_right` for a window
 * whose outer rect is (win_x, win_y, outer_w, _). Used by hit_test_at to
 * carve close/min/max regions out of HIT_TITLEBAR before returning. */
static int in_titlebar_btn(int win_x, int win_y, int outer_w,
                           int idx_from_right, int mx, int my) {
    int bx, by, bw, bh;
    titlebar_btn_rect(win_x, win_y, outer_w, idx_from_right, &bx, &by, &bw, &bh);
    return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

/* Classify a screen-space hit by walking the z-order topmost-first. The
 * console is just another z-stack entry now; whichever handle is at
 * z_order[0] wins ties at the same pixel. */
static int hit_test_at(int mx, int my, int *out_handle) {
    for (int i = 0; i < z_count; i++) {
        int h = z_order[i];
        if (is_minimized(h)) continue;
        int x, y, cw, ch;
        if (!win_get_rect(h, &x, &y, &cw, &ch)) continue;
        int ow = outer_w_dims(cw);
        int oh = outer_h_dims(ch);
        if (mx < x || mx >= x + ow || my < y || my >= y + oh) continue;
        *out_handle = h;
        /* Grip beats titlebar when they overlap at the seam. */
        if (mx >= x + ow - RESIZE_GRIP && my >= y + oh - RESIZE_GRIP) return HIT_GRIP;
        if (my < y + TITLEBAR_PX) {
            if (in_titlebar_btn(x, y, ow, 0, mx, my)) return HIT_BTN_CLOSE;
            if (in_titlebar_btn(x, y, ow, 1, mx, my)) return HIT_BTN_MAX;
            if (in_titlebar_btn(x, y, ow, 2, mx, my)) return HIT_BTN_MIN;
            return HIT_TITLEBAR;
        }
        return HIT_CLIENT;
    }
    return HIT_NONE;
}

static void clamp_to_desktop(int *x, int *y, int cw, int ch) {
    int ow = outer_w_dims(cw);
    (void)ch;
    /* Keep at least the title bar reachable for re-drag. Bottom limit also
     * subtracts the taskbar so windows can't hide their title strip behind it. */
    if (*x + ow < TITLEBAR_PX)                 *x = TITLEBAR_PX - ow;
    if (*y < 0)                                *y = 0;
    if (*x > fb_w - TITLEBAR_PX)               *x = fb_w - TITLEBAR_PX;
    if (*y > fb_h - TITLEBAR_PX - TASKBAR_PX)  *y = fb_h - TITLEBAR_PX - TASKBAR_PX;
}

/* Console resize: reallocate surface + backing buffer at new dims and copy
 * the still-visible region of the old surface into the new one so existing
 * glyphs survive. Cursor is clamped to the new grid. */
static void console_resize(int new_cw, int new_ch) {
    if (!con.enabled) return;
    /* Snap to glyph grid so cells line up without trailing fractional row. */
    int cell_w = FONT_GLYPH_W * con.scale;
    int cell_h = FONT_GLYPH_H * con.scale;
    new_cw = (new_cw / cell_w) * cell_w;
    new_ch = (new_ch / cell_h) * cell_h;
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

    int new_cols = new_cw / cell_w;
    int new_rows = new_ch / cell_h;
    char *new_cells = (char*)malloc((size_t)new_cols * (size_t)new_rows);
    char *new_saved = (char*)malloc((size_t)new_cols * (size_t)new_rows);
    if (!new_cells || !new_saved) {
        if (new_cells) free(new_cells);
        if (new_saved) free(new_saved);
        free(new_back_raw);
        free(new_raw);
        return;
    }
    memset(new_cells, 0, (size_t)new_cols * (size_t)new_rows);
    memset(new_saved, 0, (size_t)new_cols * (size_t)new_rows);

    /* Preserve the upper-left character-cell intersection. The surface is
     * then regenerated, avoiding partial glyphs at the resized edge. */
    if (con.cells) {
        int copy_cols = con.cols < new_cols ? con.cols : new_cols;
        int copy_rows = con.rows < new_rows ? con.rows : new_rows;
        for (int y = 0; y < copy_rows; y++) {
            memcpy(new_cells + y * new_cols,
                   con.cells + y * con.cols,
                   (size_t)copy_cols);
        }
    }

    if (con.raw)         free(con.raw);
    if (con_backing_raw) free(con_backing_raw);
    if (con.cells)       free(con.cells);
    if (con_saved_cells) free(con_saved_cells);

    con.surface      = new_surf;
    con.raw          = new_raw;
    con.cells        = new_cells;
    con_backing      = new_back;
    con_backing_raw  = new_back_raw;
    con_saved_cells  = new_saved;
    con.client_w     = new_cw;
    con.client_h     = new_ch;
    con.cols         = new_cols;
    con.rows         = new_rows;
    if (con.cx >= con.cols) con.cx = con.cols - 1;
    if (con.cy >= con.rows) con.cy = con.rows - 1;
    con_saved_valid  = 0;
    con_redraw();
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

/* Compute screen-space rect of titlebar button `idx_from_right` (0 = closest
 * to the corner). Used by both draw_chrome and the input pump's hit_test
 * so click rects exactly match what was rendered. */
static void titlebar_btn_rect(int win_x, int win_y, int outer_w,
                              int idx_from_right,
                              int *bx, int *by, int *bw, int *bh) {
    *bw = TB_BTN_SIZE;
    *bh = TB_BTN_SIZE;
    *bx = win_x + outer_w - BORDER_PX - TB_BTN_PAD_R
        - (idx_from_right + 1) * TB_BTN_SIZE
        - idx_from_right * TB_BTN_GAP;
    *by = win_y + (TITLEBAR_PX - TB_BTN_SIZE) / 2;
}

/* Stamp a TB_BTN_SIZE square button at (x,y). bg fills the rect; fg appears
 * only where the mask is 1. Mirrors the cursor's mask-overlay rendering. */
static void draw_button_mask(int x, int y,
                             const uint8_t mask[TB_BTN_SIZE][TB_BTN_SIZE],
                             uint32_t fg, uint32_t bg) {
    fb_fill_rect(x, y, TB_BTN_SIZE, TB_BTN_SIZE, bg);
    for (int r = 0; r < TB_BTN_SIZE; r++) {
        for (int c = 0; c < TB_BTN_SIZE; c++) {
            if (!mask[r][c]) continue;
            int px = x + c;
            int py = y + r;
            if (px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;
            *fb_pix(px, py) = fg;
        }
    }
}

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

    /* Reserve space at the right for close + max + min buttons. Title text
     * gets clamped so it never overlaps the buttons. Order from right to
     * left: close (idx 0), max (1), min (2). */
    const int n_btns = 3;
    int btn_strip_w = n_btns * TB_BTN_SIZE
                    + (n_btns - 1) * TB_BTN_GAP
                    + TB_BTN_PAD_R;

    int text_x = tb_x + 4;
    int text_y = tb_y + (tb_h - FONT_GLYPH_H) / 2;
    int avail  = tb_w - 8 - btn_strip_w;
    if (avail > 0) {
        draw_text_fb(text_x, text_y, w->title, avail, CHROME_TEXT, bg);
    }

    int bx, by, bw_, bh_;
    titlebar_btn_rect(w->x, w->y, ow, 0, &bx, &by, &bw_, &bh_);
    draw_button_mask(bx, by, btn_close_mask,     TB_BTN_FG, TB_BTN_BG);
    titlebar_btn_rect(w->x, w->y, ow, 1, &bx, &by, &bw_, &bh_);
    draw_button_mask(bx, by, btn_maximise_mask,  TB_BTN_FG, TB_BTN_BG);
    titlebar_btn_rect(w->x, w->y, ow, 2, &bx, &by, &bw_, &bh_);
    draw_button_mask(bx, by, btn_hide_mask,      TB_BTN_FG, TB_BTN_BG);
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

// #region TASKBAR

/* Enumerate the windows that should appear on the taskbar in stable order:
 * the console first (handle 0), then in-use client windows in their array
 * slot order. Order matches z-order today since neither has explicit raising. */
struct tb_entry { int handle; const char *title; };

static int build_taskbar_entries(struct tb_entry *out, int max) {
    int n = 0;
    if (con.enabled && n < max) {
        out[n].handle = HANDLE_CONSOLE;
        out[n].title  = con.title;
        n++;
    }
    for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
        if (!windows[i].in_use) continue;
        out[n].handle = windows[i].handle;
        out[n].title  = windows[i].title;
        n++;
    }
    return n;
}

static int taskbar_y(void) { return fb_h - TASKBAR_PX; }

static void draw_taskbar(void) {
    int y = taskbar_y();
    if (y < 0) return;
    fb_fill_rect(0, y, fb_w, TASKBAR_PX, TASKBAR_BG);

    struct tb_entry ents[1 + MAX_WINDOWS];
    int n = build_taskbar_entries(ents, (int)(sizeof(ents) / sizeof(ents[0])));

    int bx = TASKBAR_BTN_GAP;
    int by = y + TASKBAR_PAD_Y;
    int bh = TASKBAR_PX - 2 * TASKBAR_PAD_Y;
    for (int i = 0; i < n; i++) {
        if (bx + TASKBAR_BTN_W > fb_w) break;
        int focused  = ents[i].handle == focused_handle;
        uint32_t bg  = focused ? TASKBAR_BTN_BG_FOCUS : TASKBAR_BTN_BG;
        uint32_t fg  = focused ? TASKBAR_BTN_TEXT_FOC : TASKBAR_BTN_TEXT;
        fb_fill_rect(bx, by, TASKBAR_BTN_W, bh, bg);
        int tx = bx + 4;
        int ty = by + (bh - FONT_GLYPH_H) / 2;
        draw_text_fb(tx, ty, ents[i].title, TASKBAR_BTN_W - 8, fg, bg);
        bx += TASKBAR_BTN_W + TASKBAR_BTN_GAP;
    }
}

/* Returns 1 + writes *out_handle when the click landed on a taskbar button,
 * 0 otherwise (including clicks on the taskbar strip background). Caller
 * should still treat strip clicks as "ate the input" — the strip is opaque
 * and never belongs to a client. */
static int hit_taskbar(int mx, int my, int *out_handle) {
    int y = taskbar_y();
    if (my < y || my >= y + TASKBAR_PX) return 0;

    struct tb_entry ents[1 + MAX_WINDOWS];
    int n = build_taskbar_entries(ents, (int)(sizeof(ents) / sizeof(ents[0])));

    int bx = TASKBAR_BTN_GAP;
    int by = y + TASKBAR_PAD_Y;
    int bh = TASKBAR_PX - 2 * TASKBAR_PAD_Y;
    for (int i = 0; i < n; i++) {
        if (bx + TASKBAR_BTN_W > fb_w) break;
        if (mx >= bx && mx < bx + TASKBAR_BTN_W &&
            my >= by && my <  by + bh) {
            *out_handle = ents[i].handle;
            return 1;
        }
        bx += TASKBAR_BTN_W + TASKBAR_BTN_GAP;
    }
    return 0;
}


// #endregion TASKBAR

// #region CONSOLE (TTY DRAIN)

static void con_draw_glyph(int gx, int gy, char c) {
    int cell_w = FONT_GLYPH_W * con.scale;
    int cell_h = FONT_GLYPH_H * con.scale;
    int px = gx * cell_w;
    int py = gy * cell_h;
    if (px + cell_w > con.client_w) return;
    if (py + cell_h > con.client_h) return;
    const uint8_t *glyph;
    static const uint8_t blank[FONT_GLYPH_H] = {0};
    if (c < FONT_FIRST || c > FONT_LAST) glyph = blank;
    else                                 glyph = font8x8[(int)c - FONT_FIRST];

    if (con.scale == 1) {
        uint32_t *line = con.surface +
                         (size_t)py * (size_t)con.client_w + (size_t)px;
        for (int r = 0; r < FONT_GLYPH_H; r++) {
            memcpy(line, con_glyph_lut[glyph[r]],
                   FONT_GLYPH_W * sizeof(uint32_t));
            line += con.client_w;
        }
        return;
    }

    for (int r = 0; r < FONT_GLYPH_H; r++) {
        for (int sy = 0; sy < con.scale; sy++) {
            int y = py + r * con.scale + sy;
            uint32_t *line = con.surface +
                             (size_t)y * (size_t)con.client_w + (size_t)px;
            for (int col = 0; col < FONT_GLYPH_W; col++) {
                uint32_t color = ((glyph[r] >> col) & 1) ?
                                 CONSOLE_FG : CONSOLE_BG;
                for (int sx = 0; sx < con.scale; sx++)
                    line[col * con.scale + sx] = color;
            }
        }
    }
}

static void con_redraw(void) {
    if (!con.enabled || !con.surface || !con.cells) return;
    fill_dwords(con.surface,
                (size_t)con.client_w * (size_t)con.client_h,
                CONSOLE_BG);
    for (int y = 0; y < con.rows; y++) {
        for (int x = 0; x < con.cols; x++) {
            char c = con.cells[y * con.cols + x];
            if (c) con_draw_glyph(x, y, c);
        }
    }
    desktop_dirty = 1;
}

static int con_set_scale(int new_scale) {
    if (!con.enabled) return -1;
    if (new_scale < CON_SCALE_MIN) new_scale = CON_SCALE_MIN;
    if (new_scale > CON_SCALE_MAX) new_scale = CON_SCALE_MAX;
    if (new_scale == con.scale) return con.scale;

    int new_cols = con.client_w / (FONT_GLYPH_W * new_scale);
    int new_rows = con.client_h / (FONT_GLYPH_H * new_scale);
    if (new_cols <= 0 || new_rows <= 0) return -1;

    char *new_cells = (char*)malloc((size_t)new_cols * (size_t)new_rows);
    char *new_saved = (char*)malloc((size_t)new_cols * (size_t)new_rows);
    if (!new_cells || !new_saved) {
        if (new_cells) free(new_cells);
        if (new_saved) free(new_saved);
        return -1;
    }
    memset(new_cells, 0, (size_t)new_cols * (size_t)new_rows);
    memset(new_saved, 0, (size_t)new_cols * (size_t)new_rows);

    int copy_cols = con.cols < new_cols ? con.cols : new_cols;
    int copy_rows = con.rows < new_rows ? con.rows : new_rows;
    int src_y0 = con.cy >= copy_rows ? con.cy - copy_rows + 1 : 0;
    int dst_y0 = 0;
    for (int y = 0; y < copy_rows; y++) {
        memcpy(new_cells + (dst_y0 + y) * new_cols,
               con.cells + (src_y0 + y) * con.cols,
               (size_t)copy_cols);
    }

    int new_cx = con.cx < new_cols ? con.cx : new_cols - 1;
    int new_cy = con.cy - src_y0 + dst_y0;
    if (new_cy < 0) new_cy = 0;
    if (new_cy >= new_rows) new_cy = new_rows - 1;

    free(con.cells);
    if (con_saved_cells) free(con_saved_cells);
    con.cells = new_cells;
    con_saved_cells = new_saved;
    con.cols = new_cols;
    con.rows = new_rows;
    con.cx = new_cx;
    con.cy = new_cy;
    con.scale = new_scale;
    con_saved_valid = 0;
    con_redraw();
    return con.scale;
}

static void con_scroll(void) {
    if (con.rows <= 1) return;
    memmove(con.cells, con.cells + con.cols,
            (size_t)(con.rows - 1) * (size_t)con.cols);
    memset(con.cells + (con.rows - 1) * con.cols, 0, (size_t)con.cols);
    con_redraw();
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
    if (con.cells)
        memset(con.cells, 0, (size_t)con.cols * (size_t)con.rows);
    con.cx = con.cy = 0;
    desktop_dirty = 1;
}

static void con_save(void) {
    if (!con.enabled || !con.surface || !con_backing) return;
    size_t pixels = (size_t)con.client_w * (size_t)con.client_h;
    memcpy(con_backing, con.surface, pixels * 4);
    if (con.cells && con_saved_cells)
        memcpy(con_saved_cells, con.cells,
               (size_t)con.cols * (size_t)con.rows);
    con_saved_cx = con.cx;
    con_saved_cy = con.cy;
    con_saved_valid = 1;
}

static void con_restore(void) {
    if (!con.enabled || !con.surface || !con_backing || !con_saved_valid) return;
    size_t pixels = (size_t)con.client_w * (size_t)con.client_h;
    memcpy(con.surface, con_backing, pixels * 4);
    if (con.cells && con_saved_cells)
        memcpy(con.cells, con_saved_cells,
               (size_t)con.cols * (size_t)con.rows);
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
        if (con.cx > 0) {
            con.cx--;
            con.cells[con.cy * con.cols + con.cx] = 0;
            con_draw_glyph(con.cx, con.cy, ' ');
        }
        return;
    }
    if (c == '\t') {
        do { con_putc(' '); } while (con.cx % 8);
        return;
    }
    if (c == TTY_CTRL_CLEAR) { con_wipe(); return; }
    if (c == TTY_CTRL_PUSH)  { con_save(); con_wipe(); return; }
    if (c == TTY_CTRL_POP)   { con_restore(); return; }
    if (c == TTY_CTRL_ZOOM_IN) {
        con_set_scale(con.scale + 1);
        return;
    }

    if (c == TTY_CTRL_ZOOM_OUT) {
        con_set_scale(con.scale - 1);
        return;
    }
    if (con.cx >= con.cols) con_newline();
    con.cells[con.cy * con.cols + con.cx] = c;
    con_draw_glyph(con.cx, con.cy, c);
    con.cx++;
}

static void con_alloc(void) {
    int margin = 16;
    int cw = fb_w - 2 * margin - 2 * BORDER_PX;
    int ch = fb_h - 2 * margin - TITLEBAR_PX - BORDER_PX - TASKBAR_PX;
    if (cw < 64 || ch < 64) return;
    con.scale = CON_SCALE_MIN;
    int cell_w = FONT_GLYPH_W * con.scale;
    int cell_h = FONT_GLYPH_H * con.scale;
    cw = (cw / cell_w) * cell_w;
    ch = (ch / cell_h) * cell_h;

    size_t pixel_bytes = (size_t)cw * (size_t)ch * 4;
    size_t pages = (pixel_bytes + 4095) / 4096;
    con.surface = (uint32_t*)aligned_page_alloc(pages, &con.raw);
    if (!con.surface) return;

    fill_dwords(con.surface, (size_t)cw * (size_t)ch, CONSOLE_BG);

    /* Alt-screen backing buffer — same dimensions as the live surface so
     * memcpy push/pop is unconditional. Allocated once; never freed. */
    con_backing = (uint32_t*)aligned_page_alloc(pages, &con_backing_raw);
    if (!con_backing) {
        free(con.raw);
        con.raw = 0;
        con.surface = 0;
        return;
    }

    build_con_glyph_lut();
    con.client_w = cw;
    con.client_h = ch;
    con.cols     = cw / cell_w;
    con.rows     = ch / cell_h;
    con.cells = (char*)malloc((size_t)con.cols * (size_t)con.rows);
    con_saved_cells = (char*)malloc((size_t)con.cols * (size_t)con.rows);
    if (!con.cells || !con_saved_cells) {
        if (con.cells) free(con.cells);
        if (con_saved_cells) free(con_saved_cells);
        free(con_backing_raw);
        free(con.raw);
        con.cells = 0;
        con_saved_cells = 0;
        con_backing = 0;
        con_backing_raw = 0;
        con.surface = 0;
        con.raw = 0;
        return;
    }
    memset(con.cells, 0, (size_t)con.cols * (size_t)con.rows);
    memset(con_saved_cells, 0, (size_t)con.cols * (size_t)con.rows);
    con.x = margin;
    con.y = margin;
    con.cx = con.cy = 0;
    const char *t = "Console";
    size_t tn = 0;
    while (t[tn] && tn < sizeof(con.title) - 1) { con.title[tn] = t[tn]; tn++; }
    con.title[tn] = 0;
    con.enabled = 1;
    z_bring_to_front(HANDLE_CONSOLE);
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

/* Render one entry in the stack (real window or console). Factored out so
 * compose() can iterate z_order without caring which kind it's drawing.
 * Minimized windows are skipped entirely — they keep their slot + taskbar
 * entry but contribute no pixels until restored. */
static void compose_handle(int handle) {
    if (is_minimized(handle)) return;
    if (handle == HANDLE_CONSOLE) {
        if (!con.enabled) return;
        struct window cw = {0};
        cw.x = con.x; cw.y = con.y;
        cw.client_w = con.client_w; cw.client_h = con.client_h;
        cw.surface = con.surface;
        memcpy(cw.title, con.title, sizeof(cw.title) - 1);
        draw_chrome(&cw, focused_handle == HANDLE_CONSOLE);
        blit_surface(&cw);
        return;
    }
    struct window *w = find_handle(handle);
    if (!w) return;
    draw_chrome(w, w->handle == focused_handle);
    blit_surface(w);
}

static void compose(void) {
    fb_fill_rect(0, 0, fb_w, fb_h, DESKTOP_BG);

    /* Back-to-front so topmost overdraws everything beneath. z_order[0] is
     * the topmost handle; iterate from the tail. */
    for (int i = z_count - 1; i >= 0; i--) {
        compose_handle(z_order[i]);
    }

    /* Taskbar is always-on-top: drawn last so any window that overlaps the
     * bottom strip gets covered. clamp_to_desktop already prevents the title
     * bar from being lost behind it, but client areas may still overlap. */
    draw_taskbar();
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
    /* Handle = slot index + 1. Stable for the slot's lifetime; reused when
     * the slot is freed (find_slot reclaims it). */
    win->handle   = (int)(win - windows) + 1;
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
    z_bring_to_front(win->handle);
    desktop_dirty = 1;

    *out_client_va = client_va;
    *out_pitch     = (uint32_t)(w * 4);
    *out_handle    = win->handle;
    printf("winman: create handle=%d owner=%d %dx%d pages=%d client_va=%lx active=%d\n",
           win->handle, client_pid, w, h, (int)pages,
           (unsigned long)client_va, window_count());
    return 0;
}

/* Count of slots currently in_use. Used by the taskbar enumeration and
 * for debug logging on create/destroy/reap. */
static int window_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].in_use) n++;
    return n;
}

/* Tear down a window. Owner-check by client_pid is skipped when
 * client_pid == 0 — used by the reaper for dead-client cleanup, since the
 * dead owner can no longer issue the destroy itself. */
static void handle_destroy_internal(int handle, int client_pid_check) {
    struct window *w = find_handle(handle);
    if (!w) return;
    if (client_pid_check && w->owner_pid != client_pid_check) return;
    if (w->surface_raw) free(w->surface_raw);
    memset(w, 0, sizeof(*w));
    z_remove(handle);
    /* Refocus to the new topmost (which may be the console or another
     * window). If nothing left, fall back to 0 == console / no-focus. */
    if (focused_handle == handle) {
        focused_handle = z_count > 0 ? z_order[0] : 0;
    }
    desktop_dirty = 1;
}

static void handle_destroy(int client_pid, int handle) {
    handle_destroy_internal(handle, client_pid);
}

static void destroy_windows_for_owner(int owner_pid, const char *why) {
    if (owner_pid <= 0) return;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].in_use || windows[i].owner_pid != owner_pid) continue;
        int h = windows[i].handle;
        printf("winman: reap window %d owner_pid=%d (%s)\n",
               h, owner_pid, why ? why : "owner-exit");
        handle_destroy_internal(h, 0);
    }
}

/* True if window is currently minimized (and therefore must skip compose +
 * hit-test). Console can't be minimized. */
static int is_minimized(int handle) {
    if (handle == HANDLE_CONSOLE) return 0;
    struct window *w = find_handle(handle);
    return w && w->minimized;
}

/* Toggle min/restore. While minimized the window stays on the taskbar and
 * keeps its z-order slot, but compose + hit-test skip it; the taskbar
 * button is the only way to bring it back. */
static void toggle_minimize(int handle) {
    if (handle == HANDLE_CONSOLE) return;
    struct window *w = find_handle(handle);
    if (!w) return;
    w->minimized = !w->minimized;
    if (w->minimized && focused_handle == handle) {
        /* Refocus to next visible handle in z-order, preferring real
         * windows; falls back to console if nothing else qualifies. */
        focused_handle = 0;
        for (int i = 0; i < z_count; i++) {
            int h = z_order[i];
            if (h == handle) continue;
            if (is_minimized(h)) continue;
            focused_handle = h;
            break;
        }
    }
    desktop_dirty = 1;
}

/* Toggle maximize/restore. Saves pre-max geometry in window struct so the
 * second click restores. Uses client_window_resize so the client gets a
 * WM_RESIZE_NOTIFY with the new shared surface. */
static void client_window_resize(int handle, int new_cw, int new_ch);
static void win_set_pos(int handle, int x, int y);

static void toggle_maximize(int handle) {
    if (handle == HANDLE_CONSOLE) return;
    struct window *w = find_handle(handle);
    if (!w) return;
    if (w->maximized) {
        client_window_resize(handle, w->saved_cw, w->saved_ch);
        win_set_pos(handle, w->saved_x, w->saved_y);
        w->maximized = 0;
    } else {
        w->saved_x  = w->x;        w->saved_y  = w->y;
        w->saved_cw = w->client_w; w->saved_ch = w->client_h;
        int max_cw = fb_w - 2 * BORDER_PX;
        int max_ch = fb_h - TASKBAR_PX - TITLEBAR_PX - BORDER_PX;
        win_set_pos(handle, 0, 0);
        client_window_resize(handle, max_cw, max_ch);
        w->maximized = 1;
    }
    desktop_dirty = 1;
}

/* Walk the kernel proc table and reap any window whose owner is explicitly
 * terminal. Missing owner rows are ignored here: the kernel sends
 * IPC_PEER_EXITED from task_exit(), so absence in a snapshot is treated as
 * inconclusive rather than permission to destroy visible client state.
 *
 * Note: this does NOT unmap the shared pages from the (already-gone) owner's
 * address space. The kernel reclaims that pml4 when the task struct is
 * freed; the phys frames go back to PMM with it. */
static void reap_dead_windows(void) {
    /* MAX_WINDOWS is small but proc_list may report many tasks. Snapshot a
     * fixed-size table; if there are more procs than fit we just skip the
     * tail (worst case: late reap, not corruption). */
    struct proc_info procs[32];
    long n = proc_list(procs, (long)(sizeof(procs) / sizeof(procs[0])));
    if (n < 0) return;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        int owner = windows[i].owner_pid;
        int terminal = 0;
        for (long j = 0; j < n; j++) {
            if (procs[j].pid != owner) continue;
            int s = procs[j].state;
            terminal = (s == PROC_STATE_ZOMBIE || s == PROC_STATE_DEAD);
            break;
        }
        if (terminal) {
            int h = windows[i].handle;
            printf("winman: reap window %d owner_pid=%d (dead)\n", h, owner);
            handle_destroy_internal(h, 0);
        }
    }
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
        case IPC_PEER_EXITED:
            destroy_windows_for_owner(m.a, "peer-exited");
            break;
        default:
            break;
        }
    }
}


// #endregion IPC PUMP

// #region INPUT PUMP

/* Translate input event from screen coords to the focused window's
 * client-area coords before forwarding. Clients draw into a surface that
 * starts at (0,0) so they shouldn't have to know their own position on
 * the desktop — winman is the only thing that does. */
static void forward_input(int target_pid, int win_handle, const struct msg *m) {
    if (target_pid <= 0) return;

    int wx = 0, wy = 0, wcw = 0, wch = 0;
    int local_x = m->x;
    int local_y = m->y;
    if (win_get_rect(win_handle, &wx, &wy, &wcw, &wch)) {
        /* Client area starts at (wx + BORDER_PX, wy + TITLEBAR_PX). */
        local_x = m->x - (wx + BORDER_PX);
        local_y = m->y - (wy + TITLEBAR_PX);
    }

    struct ipc_msg out;
    memset(&out, 0, sizeof(out));
    out.type = IPC_WM_INPUT;
    out.a    = m->type;
    out.b    = m->param;
    out.c    = local_x;
    out.d    = local_y;
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
            /* Taskbar wins over any window underneath: it's drawn always-on-top
             * and its strip is opaque, so a click in the strip never belongs to
             * a client even if a window's client area overlaps it. */
            if (m.y >= taskbar_y()) {
                int tb_handle = 0;
                if (hit_taskbar(m.x, m.y, &tb_handle)) {
                    /* Taskbar click on a minimized window restores it. */
                    struct window *wt = (tb_handle != HANDLE_CONSOLE)
                                        ? find_handle(tb_handle) : 0;
                    if (wt && wt->minimized) wt->minimized = 0;
                    focused_handle = tb_handle;
                    z_bring_to_front(tb_handle);
                    desktop_dirty  = 1;
                }
                /* Strip background also eats the click — don't forward. */
                continue;
            }
            int hit_handle = 0;
            int kind = hit_test_at(m.x, m.y, &hit_handle);
            if (kind == HIT_BTN_CLOSE) {
                /* User-initiated close. Skip owner-check (UI is privileged).
                 * Console is built-in: refuse to destroy it. */
                if (hit_handle != HANDLE_CONSOLE) {
                    printf("winman: close button -> destroy handle=%d\n",
                           hit_handle);
                    handle_destroy_internal(hit_handle, 0);
                }
                continue;
            }
            if (kind == HIT_BTN_MAX) {
                toggle_maximize(hit_handle);
                continue;
            }
            if (kind == HIT_BTN_MIN) {
                toggle_minimize(hit_handle);
                continue;
            }
            if (kind == HIT_TITLEBAR || kind == HIT_GRIP) {
                int x, y, cw, ch;
                if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
                    focused_handle = hit_handle;
                    z_bring_to_front(hit_handle);
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
                z_bring_to_front(hit_handle);
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
        if (focus) forward_input(focus->owner_pid, focus->handle, &m);
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
    focused_handle = 0;
    con_alloc();
    printf("winman: con.enabled=%d surface=%p w=%d h=%d\n",
           con.enabled, (void*)con.surface, con.client_w, con.client_h);

    int self_pid = (int)get_pid();
    int tick = 0;
    int32_t last_cx = 0, last_cy = 0;
    int     have_last = 0;

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

        /* Advance once per iteration, not once per repaint. This used to
         * live inside the `desktop_dirty && (++tick % 2)` test below, where
         * short-circuit evaluation froze it on an idle desktop — turning
         * the reap check into a constant that either fired every frame or
         * never fired at all, depending on where it stopped. */
        tick++;

        /* Reap windows whose owners have died without sending DESTROY_REQ.
         * Hot path runs the syscall (proc_list) ~every 64 ticks so the
         * common case stays cheap. */
        if ((tick & 63) == 0) reap_dead_windows();

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

        /* Halve the repaint rate: a dirty desktop composites on even ticks. */
        int recompose = desktop_dirty && (tick % 2 == 0);
        if (recompose) {
            compose();
            memcpy(fb_hw, fb, fb_bytes);
            fb_damage(0, 0, (uint32_t)fb_w, (uint32_t)fb_h);
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

        yield();
    }
    return 0;
}

// #endregion MAIN
