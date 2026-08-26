#include <lib/bmp.h>
#include <lib/gfx.h>
#include <lib/keymap.h>
#include <lib/syscall.h>
#include <lib/ttf.h>
#include <lib/wm.h>
#include <display/fonts/font8x8.h>
#include <include/sys/types.h>
#include <include/time.h>
#include <stddef.h>
#include <stdint.h>

/* In-band TTY control codes , must match kernel/display/tty.h. Defined
 * inline rather than #include'd because that header also declares
 * tty_drain() with a `size_t` signature that conflicts with the userspace
 * `long` wrapper in lib/syscall.h. */
#define TTY_CTRL_CLEAR 0x0C
#define TTY_CTRL_PUSH 0x1C
#define TTY_CTRL_POP 0x1D
#define TTY_CTRL_ZOOM_IN 0x1E
#define TTY_CTRL_ZOOM_OUT 0x1F

struct program {
  const char *name;
  const char *path;
};

struct desktop_icon {
  int x, y, w, h;
  struct program program;
  struct bmp_image icon;
  int loaded;
};

struct start_menu_item {
  struct program *programs;
};

#define MAX_ICONS 32
static struct desktop_icon desktop_icons[MAX_ICONS];
static int desktop_icon_count = 0;

/* Desktop Wallpaper */
static struct bmp_image wallpaper_img;
static int wallpaper_loaded = 0;

/* Start Menu */

#define START_MENU_W 160
#define START_MENU_ITEM_H 24
#define START_MENU_PAD 4

/* Pinned entries, always first and always in this order. Their labels are
 * nicer than a filename and their presence does not depend on what happens
 * to be packaged, so a missing binary shows up as a failed spawn rather
 * than a silently absent menu item. */
static const struct program start_menu_defaults[] = {
  {"Shelf (Shell)", "system/bin/sh.elf"},
  {"Desk Elf", "system/bin/deskelf.elf"},
  {"Text Editor", "system/bin/notepad.elf"},
  {"About", "system/bin/about.elf"}
};

#define START_MENU_DEFAULT_COUNT                                               \
  (int)(sizeof(start_menu_defaults) / sizeof(start_menu_defaults[0]))

/* Everything else is discovered by scanning the executable directories at
 * startup, so the menu tracks whatever is actually on the volume instead of
 * a hardcoded list that goes stale the moment binaries move. Names and paths
 * are copied into the entry because the directory buffer they came from is
 * reused by the next read. */
static const char *const start_menu_scan_dirs[] = {
  "system/bin",
  "usr/bin",
  "usr/local/bin",
};

#define START_MENU_SCAN_DIR_COUNT                                              \
  (int)(sizeof(start_menu_scan_dirs) / sizeof(start_menu_scan_dirs[0]))

#define START_MENU_MAX 16
#define START_MENU_NAME_MAX 32
#define START_MENU_PATH_MAX 80

struct start_entry {
  char name[START_MENU_NAME_MAX];
  char path[START_MENU_PATH_MAX];
};

static struct start_entry start_menu_programs[START_MENU_MAX];
static int start_menu_count = 0;
static int start_menu_open = 0;
static int start_menu_hover = -1; // Index of hovered item, -1 if none


extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void *memmove(void *, const void *, size_t);
extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);
extern void *malloc(size_t);
extern void free(void *);
#include <stdio.h>

/* Forward decls , used by helpers that appear before their definitions
 * because GEOMETRY + DRAG sit ahead of the helper bag. */
struct window;
static void *aligned_page_alloc(size_t npages, void **out_raw);
static int window_count(void);
static int is_minimized(int handle);
static struct window *find_handle(int handle);
static void con_redraw(void);
static void titlebar_btn_rect(int win_x, int win_y, int outer_w,
                              int idx_from_right, int *bx, int *by, int *bw,
                              int *bh);

/* Modal prompt. Defined down with the other IPC handlers but drawn from
 * compose() and driven from pump_input(), both of which come earlier. */
static void draw_prompt(void);
static void prompt_abandon_for_owner(int owner_pid);
static int prompt_handle_key(int keycode, int shift);
static int prompt_handle_click(int mx, int my);

#define CURSOR_BMP_PATH "system/icons/cursor.bmp"

#define CURSOR_W 12
#define CURSOR_H 12
#define CURSOR_MAX_SOURCE_DIM 32
#define CURSOR_MAX_SCALE 4
#define CURSOR_MAX_DRAW_DIM (CURSOR_MAX_SOURCE_DIM * CURSOR_MAX_SCALE)
#define COLOR_BORDER 0x00000000u
#define COLOR_FILL 0x00FFFFFFu

static struct bmp_image cursor_img; /* pixels == 0 until a load succeeds */
static uint32_t cursor_under[CURSOR_MAX_DRAW_DIM * CURSOR_MAX_DRAW_DIM];

static const uint8_t fallback_cursor_mask[CURSOR_H][CURSOR_W] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0},
    {1, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0}, {1, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

#define DESKTOP_BG 0x00008080u /* teal , Win3-ish                        */
#define CHROME_BG 0x00C0C0C0u
#define TITLEBAR_FG 0x000000A0u /* navy title bar (focused)               */
#define TITLEBAR_BG 0x00808080u
#define CHROME_TEXT 0x00FFFFFFu
#define CONSOLE_FG 0x00FFFFFFu
#define CONSOLE_BG 0x00000000u

#define MENU_BG       TASKBAR_BTN_BG
#define MENU_TEXT     TASKBAR_BTN_TEXT
#define MENU_HOVER_BG TASKBAR_BTN_BG_FOCUS
#define MENU_HOVER_FG TASKBAR_BTN_TEXT_FOC

#define BORDER_PX 1
#define TITLEBAR_PX 16

/* Status strip along the bottom of a window frame. Opt-in per window at
 * create time (WM_CREATE_STATUSBAR): a window that never sets status text
 * would otherwise pay for a blank strip, and the console does not want one.
 * The client area is unchanged , the frame grows , so opting in never costs
 * an app drawing space. */
#define STATUSBAR_PX 16
#define STATUSBAR_BG 0x00C0C0C0u
#define STATUSBAR_FG 0x00000000u
#define STATUSBAR_PAD_X 4

/* Modal prompt dialog. Winman renders it centred on the desktop and drives
 * it; the requesting app is blocked in wm_prompt() and sees no input. */
#define PROMPT_W 320
#define PROMPT_H 120
#define PROMPT_PAD 10
#define PROMPT_BTN_W 76
#define PROMPT_BTN_H 20
#define PROMPT_BTN_GAP 8
#define PROMPT_FIELD_H 20
#define PROMPT_MAX_TEXT 47
#define PROMPT_BG 0x00C0C0C0u
#define PROMPT_TEXT 0x00000000u
#define PROMPT_FIELD_BG 0x00FFFFFFu
#define PROMPT_BTN_BG 0x00D4D0C8u
#define PROMPT_BTN_SEL_BG 0x000000A0u
#define PROMPT_BTN_SEL_FG 0x00FFFFFFu

/* Taskbar: always-on-top strip pinned to the bottom of the desktop.
 * Buttons list the console + every client window; clicking a button focuses
 * that handle. */
#define TASKBAR_PX 24
#define TASKBAR_BG 0x00808080u
#define TASKBAR_BTN_BG 0x00C0C0C0u
#define TASKBAR_BTN_BG_FOCUS 0x000000A0u
#define TASKBAR_BTN_TEXT 0x00000000u
#define TASKBAR_BTN_TEXT_FOC 0x00FFFFFFu
#define TASKBAR_BTN_W 120
#define TASKBAR_BTN_GAP 2
#define TASKBAR_PAD_Y 2
#define TASKBAR_START_W                                                        \
  24 // size of the taskbar start button, should be similar to old NT / Windows

/* Clock, right-aligned in the taskbar: time above date, the way Windows has
 * always done it. Width is set by the date, the longer of the two lines:
 * "17/08/2026" is 10 glyphs at FONT_GLYPH_W (8) = 80px, plus a little
 * breathing room. Too narrow and draw_text_fb silently clips the year. */
#define CLOCK_W 88
#define CLOCK_PAD_R 6
#define CLOCK_LINE_GAP 2
#define CLOCK_FG 0x00FFFFFFu
/* Poll interval. The taskbar only shows minutes, so once a second is already
 * far more often than the display can change , it just keeps the rollover
 * from lagging by up to a minute. */
#define CLOCK_POLL_TICKS 100u

/* Drag affordances. RESIZE_GRIP = size of the bottom-right square that acts
 * as the resize handle. MIN_CLIENT_* = floor below which we refuse to shrink
 * (smaller and the chrome geometry would invert). */
#define RESIZE_GRIP 12
#define MIN_CLIENT_W 64
#define MIN_CLIENT_H 40
#define CON_SCALE_MIN 1
#define CON_SCALE_MAX 4
#define CON_TTF_PATH "/system/fonts/sansdisplayvariable.ttf"
#define CON_TTF_PX 16

/* Hit-test region codes returned by hit_test_at. */
#define HIT_NONE 0
#define HIT_TITLEBAR 1
#define HIT_GRIP 2
#define HIT_CLIENT 3
#define HIT_BTN_CLOSE 4
#define HIT_BTN_MAX 5
#define HIT_BTN_MIN 6
#define HIT_START_MENU 7
#define HIT_START_BTN  8
#define HIT_TASKBAR_BTN 9
/* Status strip: opaque chrome, so it swallows the click rather than
 * forwarding it to the client at coordinates outside the client surface. */
#define HIT_STATUSBAR 10
#define HIT_PROMPT 11
/* Titlebar buttons: small square icons right-anchored in the titlebar.
 * Today only the close (X) button exists; min/max can slot in by bumping
 * `idx_from_right` in titlebar_btn_rect. Buttons render as bitmap masks
 * (same scheme as the cursor) so future icon swaps don't need new helpers. */
#define TB_BTN_SIZE 12
#define TB_BTN_GAP 2
#define TB_BTN_PAD_R 2
#define TB_BTN_BG 0x00C0C0C0u
#define TB_BTN_BG_HOVER 0x00FF6060u
#define TB_BTN_FG 0x00000000u

/* Taskbar start button artwork. The image is scaled to fit the button, so
 * the source does not have to be TASKBAR_START_W square , but reject
 * anything absurd so a mis-sized file can't turn into a huge rescale. */
#define TB_START_ICON_PATH "/system/icons/icon.bmp"
#define TB_START_ICON_MAX_DIM 256
#define TB_START_ICON_PAD 2


static const uint8_t fallback_taskbar_start_mask[24][24] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
};

/* title-bar button glyphs. 0 = background (titlebar btn bg), 1 = foreground.
 * Drawn through draw_button_mask which fills the rect with `bg` first then
 * stamps `fg` only where the mask is 1. */
static const uint8_t fallback_btn_close_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0}, {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0}, {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0},
    {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static const uint8_t fallback_btn_maximise_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static const uint8_t fallback_btn_hide_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

/* Special handle reserved for the built-in console. Real client window
 * handles are derived from their slot index: handle = (slot_index + 1),
 * so they always live in 1..MAX_WINDOWS and get reused as soon as the
 * slot is freed. No monotonically-growing counter. */
#define HANDLE_CONSOLE 0

static uint32_t *fb_hw;
static uint32_t *fb;           /* page-aligned compositor backbuffer */
static size_t fb_bytes;        /* bytes in the current visible row span */
static size_t fb_capacity;     /* allocated compositor-buffer bytes */
static size_t fb_mapped_bytes; /* framebuffer prefix mapped in this task */
static int fb_registered;      /* kernel has cached the current backbuffer */
static int fb_w, fb_h, fb_stride;
static int desktop_dirty = 0;
static int dirty_x = 0, dirty_y = 0, dirty_w = 0, dirty_h = 0;

#define MAX_WINDOWS 8

struct window {
  uint32_t *surface;  /* page-aligned, owned by winman          */
  void *surface_raw;  /* original malloc ptr for free()         */
  uint64_t client_va; /* va in owner_pml4 (0 if not shared)     */

  int in_use;
  int handle;
  int owner_pid;

  int x, y;
  int client_w, client_h;

  int n_pages;

  /* Title-bar button state. minimized: skip compose + hit-test, but stay
   * on taskbar; clicking the taskbar button restores. maximized: window
   * fills the desktop (above taskbar); pre-max geometry is saved here so
   * a second click restores. */
  int minimized;
  int maximized;
  int saved_x, saved_y;
  int saved_cw, saved_ch;

  /* Height of this window's status strip, 0 when it has none. Kept as a
   * height rather than a flag so every geometry helper can add it blindly. */
  int status_h;
  char status[48];

  char title[48];
};

/* One modal dialog at a time, owned by winman rather than by any window.
 * `owner_pid` is who gets the reply; `owner_handle` is what it centres on
 * and what dies with it if the window goes away mid-prompt. */
struct prompt_state {
  int active;
  int kind;
  int owner_pid;
  int owner_handle;
  /* Screen rect, fixed when the dialog opens. Drawing, hit-testing and the
   * erase-on-dismiss all have to agree on where the dialog is, so this is
   * stored rather than recomputed , the owner window it is centred on can
   * be gone by the time the dialog is torn down. */
  int x, y, w, h;
  int selected; /* index into the kind's button row */
  int caret;    /* text length for WM_PROMPT_TEXT   */
  char message[48];
  char input[PROMPT_MAX_TEXT + 1];
};

struct drag_state {
  int active;
  int kind;   /* HIT_TITLEBAR (move) or HIT_GRIP (resize) */
  int handle; /* HANDLE_CONSOLE or a window handle        */
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
 * last, hit-tested first). Console takes a slot too , it can be raised
 * over client windows just like any other surface. Re-bound on every
 * create / focus / destroy so the array always reflects current stacking. */
#define MAX_Z (1 + MAX_WINDOWS)
static int z_order[MAX_Z];
static int z_count = 0;

struct console {
    struct window win;

    char *cells;
    char *saved_cells;
    int cols, rows;
    int cx, cy;
    int scale;
};

static struct console con;

/* Alt-screen backing buffer for console push/pop. Allocated once at
 * con_alloc time, same size as the live surface. saved_valid gates pop so
 * a spurious pop without a prior push is a no-op. */
static uint32_t *con_backing = 0;
static void *con_backing_raw = 0;
static char *con_saved_cells = 0;
static int con_saved_cx = 0;
static int con_saved_cy = 0;
static int con_saved_valid = 0;
