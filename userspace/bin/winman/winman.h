#include "../../lib/syscall.h"
#include "../../lib/wm.h"
#include "../../lib/bmp.h"
#include "../../lib/gfx.h"
#include "../../lib/ttf.h"
#include "display/fonts/font8x8.h"
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

#define CURSOR_BMP_PATH "/cursor.bmp"

#define CURSOR_W 12
#define CURSOR_H 12
#define COLOR_BORDER 0x00000000u
#define COLOR_FILL   0x00FFFFFFu

static struct bmp_image cursor_img;   /* pixels == 0 until a load succeeds */

static const uint8_t fallback_cursor_mask[CURSOR_H][CURSOR_W] = {
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
#define CON_TTF_PATH "/system/fonts/sansdisplayvariable.ttf"
#define CON_TTF_PX   16

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
static const uint8_t fallback_btn_close_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
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

static const uint8_t fallback_btn_maximise_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
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

static const uint8_t fallback_btn_hide_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
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

static uint32_t *fb_hw;
static uint32_t *fb;            /* back buffer (page-aligned malloc) */
static void     *fb_raw;
static size_t    fb_bytes;
static int       fb_w, fb_h, fb_stride;
static int       desktop_dirty = 1;

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
