#ifndef WINMAN_INTERNAL_H
#define WINMAN_INTERNAL_H

#include <display/fonts/font8x8.h>
#include <include/sys/types.h>
#include <include/time.h>
#include <lib/bmp.h>
#include <lib/damage.h>
#include <lib/gfx.h>
#include <lib/keymap.h>
#include <lib/page_alloc.h>
#include <lib/syscall.h>
#include <lib/ttf.h>
#include <lib/wm.h>
#include <stddef.h>
#include <stdint.h>

#ifdef WINMAN_DECLARE_STATE
#define WINMAN_STATE extern
#define WINMAN_STATE_INIT(value)
#else
#define WINMAN_STATE
#define WINMAN_STATE_INIT(value) = value
#endif

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

struct tb_entry {
  int handle;
  const char *title;
};

#define MAX_ICONS 32
WINMAN_STATE struct desktop_icon desktop_icons[MAX_ICONS];
WINMAN_STATE int desktop_icon_count WINMAN_STATE_INIT(0);

/* Desktop Wallpaper */
WINMAN_STATE struct bmp_image wallpaper_img;
WINMAN_STATE int wallpaper_loaded WINMAN_STATE_INIT(0);

/* Start Menu */

#define START_MENU_W 160
#define START_MENU_ITEM_H 24
#define START_MENU_PAD 4

#define DOUBLE_CLICK_TICKS 370u
#define DOUBLE_CLICK_SLOP 4
#define CLIENT_DIM_HARD_LIMIT 2048

/* Pinned entries, always first and always in this order. Their labels are
 * nicer than a filename and their presence does not depend on what happens
 * to be packaged, so a missing binary shows up as a failed spawn rather
 * than a silently absent menu item. */
#ifndef WINMAN_DECLARE_STATE
const struct program start_menu_defaults[] = {
    {"Shelf (Shell)", "system/bin/sh.elf"},
    {"Desk Elf", "system/bin/deskelf.elf"},
    {"Text Editor", "system/bin/notepad.elf"},
    {"About", "system/bin/about.elf"}};

#else
extern const struct program start_menu_defaults[4];
#endif

#define START_MENU_DEFAULT_COUNT                                               \
  (int)(sizeof(start_menu_defaults) / sizeof(start_menu_defaults[0]))

/* Everything else is discovered by scanning the executable directories at
 * startup, so the menu tracks whatever is actually on the volume instead of
 * a hardcoded list that goes stale the moment binaries move. Names and paths
 * are copied into the entry because the directory buffer they came from is
 * reused by the next read. */
#ifndef WINMAN_DECLARE_STATE
const char *start_menu_scan_dirs[] = {
    "system/bin",
    "usr/bin",
    "usr/local/bin",
};

#else
extern const char *const start_menu_scan_dirs[3];
#endif

#define START_MENU_SCAN_DIR_COUNT                                              \
  (int)(sizeof(start_menu_scan_dirs) / sizeof(start_menu_scan_dirs[0]))

#define START_MENU_MAX 16
#define START_MENU_NAME_MAX 32
#define START_MENU_PATH_MAX 80

struct start_entry {
  char name[START_MENU_NAME_MAX];
  char path[START_MENU_PATH_MAX];
};

WINMAN_STATE struct start_entry start_menu_programs[START_MENU_MAX];
WINMAN_STATE int start_menu_count WINMAN_STATE_INIT(0);
WINMAN_STATE int start_menu_open WINMAN_STATE_INIT(0);
WINMAN_STATE int start_menu_hover WINMAN_STATE_INIT(-1);

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
struct console;
int window_count(void);
int is_minimized(int handle);
struct window *find_handle(int handle);
struct console *con_for_handle(int handle);
void con_redraw(struct console *c);
void titlebar_btn_rect(int win_x, int win_y, int outer_w, int idx_from_right,
                       int *bx, int *by, int *bw, int *bh);

/* Modal prompt. Defined down with the other IPC handlers but drawn from
 * compose() and driven from pump_input(), both of which come earlier. */
void draw_prompt(void);
void prompt_abandon_for_owner(int owner_pid);
int prompt_handle_key(int keycode, int shift);
int prompt_handle_click(int mx, int my);

#define CURSOR_BMP_PATH "system/icons/cursor.bmp"

#define CURSOR_W 12
#define CURSOR_H 12
#define CURSOR_MAX_SOURCE_DIM 32
#define CURSOR_MAX_SCALE 4
#define CURSOR_MAX_DRAW_DIM (CURSOR_MAX_SOURCE_DIM * CURSOR_MAX_SCALE)
#define COLOR_BORDER 0x00000000u
#define COLOR_FILL 0x00FFFFFFu

WINMAN_STATE struct bmp_image cursor_img;
WINMAN_STATE uint32_t cursor_under[CURSOR_MAX_DRAW_DIM * CURSOR_MAX_DRAW_DIM];

#ifndef WINMAN_DECLARE_STATE
const uint8_t fallback_cursor_mask[CURSOR_H][CURSOR_W] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0},
    {1, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0}, {1, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};
#else
extern const uint8_t fallback_cursor_mask[CURSOR_H][CURSOR_W];
#endif

#define DESKTOP_BG 0x00008080u /* teal , Win3-ish                        */
#define CHROME_BG 0x00C0C0C0u
#define TITLEBAR_FG 0x000000A0u /* navy title bar (focused)               */
#define TITLEBAR_BG 0x00808080u
#define CHROME_TEXT 0x00FFFFFFu
#define CONSOLE_FG 0x00FFFFFFu
#define CONSOLE_BG 0x00000000u

#define MENU_BG TASKBAR_BTN_BG
#define MENU_TEXT TASKBAR_BTN_TEXT
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
#define HIT_START_BTN 8
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

#ifndef WINMAN_DECLARE_STATE
const uint8_t fallback_taskbar_start_mask[24][24] = {
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
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
#else
extern const uint8_t fallback_taskbar_start_mask[24][24];
#endif

/* title-bar button glyphs. 0 = background (titlebar btn bg), 1 = foreground.
 * Drawn through draw_button_mask which fills the rect with `bg` first then
 * stamps `fg` only where the mask is 1. */
#ifndef WINMAN_DECLARE_STATE
const uint8_t fallback_btn_close_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0}, {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0}, {0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0},
    {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};
#else
extern const uint8_t fallback_btn_close_mask[TB_BTN_SIZE][TB_BTN_SIZE];
#endif

#ifndef WINMAN_DECLARE_STATE
const uint8_t fallback_btn_maximise_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};
#else
extern const uint8_t fallback_btn_maximise_mask[TB_BTN_SIZE][TB_BTN_SIZE];
#endif

#ifndef WINMAN_DECLARE_STATE
const uint8_t fallback_btn_hide_mask[TB_BTN_SIZE][TB_BTN_SIZE] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};
#else
extern const uint8_t fallback_btn_hide_mask[TB_BTN_SIZE][TB_BTN_SIZE];
#endif

/* Handles reserved for the built-in consoles. Real client window handles are
 * derived from their slot index: handle = (slot_index + 1), so they always
 * live in 1..MAX_WINDOWS and get reused as soon as the slot is freed. No
 * monotonically-growing counter.
 *
 * Console handles sit in their own block above that range, one per TTY
 * channel. The base is deliberately not 0: 0 means "nothing focused", and
 * when the console owned that value every "is anything focused" test had to
 * name it as an exception.
 *
 * CON_MAX must not exceed TTY_MAX in kernel/display/tty.h , a console
 * without a channel behind it has nothing to render. */
#define CON_MAX 4
#define HANDLE_CONSOLE_BASE 128
#define HANDLE_CONSOLE HANDLE_CONSOLE_BASE /* the boot console, on TTY 0 */

WINMAN_STATE uint32_t *fb_hw;
WINMAN_STATE uint32_t *fb;
WINMAN_STATE size_t fb_bytes;
WINMAN_STATE size_t fb_capacity;
WINMAN_STATE size_t fb_mapped_bytes;
WINMAN_STATE int fb_registered;
WINMAN_STATE int fb_w, fb_h, fb_stride;
WINMAN_STATE struct gfx_damage desktop_damage;

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
WINMAN_STATE struct drag_state drag;

WINMAN_STATE struct window windows[MAX_WINDOWS];
WINMAN_STATE int focused_handle WINMAN_STATE_INIT(0);

/* Z-order: handles ordered front-to-back. z_order[0] is topmost (drawn
 * last, hit-tested first). Consoles take slots too , they can be raised
 * over client windows just like any other surface. Re-bound on every
 * create / focus / destroy so the array always reflects current stacking. */
#define MAX_Z (CON_MAX + MAX_WINDOWS)
WINMAN_STATE int z_order[MAX_Z];
WINMAN_STATE int z_count WINMAN_STATE_INIT(0);

/* One console window mirroring one kernel TTY channel. The shell bound to
 * `tty` writes there and reads its keystrokes from there, so two consoles
 * never see each other's text or input.
 *
 * `pid` is the shell winman started on this console. It is what the close
 * button kills, and what the reaper watches: when it dies (the user typed
 * `exit`), the console goes with it. */
struct console {
  struct window win;

  int tty;
  int pid;

  char *cells;
  int cols, rows;
  int cx, cy;
  int scale;

  /* Alt-screen buffers for push/pop, allocated with the surface and the
   * same size as it. saved_valid gates pop so a spurious pop without a
   * prior push is a no-op. */
  uint32_t *backing;
  void *backing_raw;
  char *saved_cells;
  int saved_cx, saved_cy;
  int saved_valid;
};

/* cons[0] mirrors TTY_KERNEL and is the boot console: it is always present,
 * its shell was started by the kernel rather than by winman, and closing it
 * is allowed , the kernel no longer depends on that shell being alive. */
WINMAN_STATE struct console cons[CON_MAX];

/* A handful of event-loop values are shared with lifecycle and prompt
 * modules. They remain private to the Winman binary despite external C
 * linkage because this header is not installed as a userspace API. */
extern int close_pending_handle;
extern u32 close_pending_tick;
extern struct prompt_state prompt;
extern int shift_held;
extern int ctrl_held;

#include "winman_prototypes.h"

#undef WINMAN_STATE
#undef WINMAN_STATE_INIT

#endif
