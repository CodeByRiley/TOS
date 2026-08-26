#include "winman.h"
#include "key_codes.h"
#include "syscall.h"
#include <display/print.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static int cursor_w(void) {
  return cursor_img.pixels ? cursor_img.width : CURSOR_W;
}

static int cursor_h(void) {
  return cursor_img.pixels ? cursor_img.height : CURSOR_H;
}

static int alt_pressed = 0;

/* Double-click tracking */
static int last_icon_clicked = -1;
static u32 last_icon_click_tick = 0;
static int last_title_clicked = -1;
static u32 last_title_click_tick = 0;
static int last_title_click_x = 0;
static int last_title_click_y = 0;

#define DOUBLE_CLICK_TICKS 370u
#define DOUBLE_CLICK_SLOP 4
#define CLIENT_DIM_HARD_LIMIT 2048

/* Close escalation. The close button is a request — the owner is expected to
 * tear its own window down so it can prompt about unsaved work first. An app
 * that ignores WM_EV_QUIT or has wedged would otherwise be unclosable, so a
 * second click while the first request is still outstanding destroys the
 * window here instead of asking again.
 *
 * The window is bounded in time as well as cleared on destroy: handles are
 * slot indices and get reused, so without a deadline a stale pending request
 * could force-destroy an unrelated window that later inherited the handle.
 * Ticks come from the PIT at 500 Hz, so this is roughly three seconds. */
#define CLOSE_ESCALATE_TICKS 1500u
static int close_pending_handle = -1;
static u32 close_pending_tick = 0;

static int max_client_w = MIN_CLIENT_W;
static int max_client_h = MIN_CLIENT_H;

/* Recompute the largest client area that can fit above the taskbar. Keep the
 * allocator's existing hard limit as a second ceiling, and retain the minimum
 * valid chrome dimensions for unusually small framebuffer modes. */
static void update_client_size_limits(void) {
  int new_max_w = fb_w - 2 * BORDER_PX;
  int new_max_h = fb_h - TASKBAR_PX - TITLEBAR_PX - BORDER_PX;

  if (new_max_w > CLIENT_DIM_HARD_LIMIT)
    new_max_w = CLIENT_DIM_HARD_LIMIT;
  if (new_max_h > CLIENT_DIM_HARD_LIMIT)
    new_max_h = CLIENT_DIM_HARD_LIMIT;
  if (new_max_w < MIN_CLIENT_W)
    new_max_w = MIN_CLIENT_W;
  if (new_max_h < MIN_CLIENT_H)
    new_max_h = MIN_CLIENT_H;

  max_client_w = new_max_w;
  max_client_h = new_max_h;
}

static void clamp_client_size(int *client_w, int *client_h) {
  if (*client_w < MIN_CLIENT_W)
    *client_w = MIN_CLIENT_W;
  if (*client_h < MIN_CLIENT_H)
    *client_h = MIN_CLIENT_H;
  if (*client_w > max_client_w)
    *client_w = max_client_w;
  if (*client_h > max_client_h)
    *client_h = max_client_h;
}

static void reset_titlebar_click(void) {
  last_title_clicked = -1;
  last_title_click_tick = 0;
}

/* Consume a second click on the same titlebar when it is close enough in both
 * time and position. Reset after a match so a triple-click only toggles once.
 */
static int titlebar_click_is_double(int handle, int x, int y, u32 now) {
  int dx = x - last_title_click_x;
  int dy = y - last_title_click_y;
  if (dx < 0)
    dx = -dx;
  if (dy < 0)
    dy = -dy;

  if (handle == last_title_clicked &&
      now - last_title_click_tick < DOUBLE_CLICK_TICKS &&
      dx <= DOUBLE_CLICK_SLOP && dy <= DOUBLE_CLICK_SLOP) {
    reset_titlebar_click();
    return 1;
  }

  last_title_clicked = handle;
  last_title_click_tick = now;
  last_title_click_x = x;
  last_title_click_y = y;
  return 0;
}

/* Try the on-disk cursor once at startup. Failure is not an error worth
 * stopping for — it just leaves the built-in mask in place. */
static void cursor_load(void) {
  if (bmp_load(CURSOR_BMP_PATH, &cursor_img) == 0) {
    if (cursor_img.width > 0 && cursor_img.height > 0 &&
        cursor_img.width <= CURSOR_MAX_SOURCE_DIM &&
        cursor_img.height <= CURSOR_MAX_SOURCE_DIM) {
      printf("winman: cursor %dx%d from %s\n", cursor_img.width,
             cursor_img.height, CURSOR_BMP_PATH);
    } else {
      printf(
          "winman: %s has invalid cursor dimensions, using built-in cursor\n",
          CURSOR_BMP_PATH);
      bmp_free(&cursor_img);
    }
  } else {
    printf("winman: %s unavailable, using built-in cursor\n", CURSOR_BMP_PATH);
  }
}

static inline void blend_px(uint32_t *dst, uint32_t src);

static void desktop_load(void) {
  if (bmp_load("/system/wallpaper.bmp", &wallpaper_img) == 0) {
    wallpaper_loaded = 1;
    printf("winman: wallpaper %dx%d loaded\n", wallpaper_img.width,
           wallpaper_img.height);
  }

  /* Curated, unlike the start menu: each entry needs artwork at
   * /system/icons/<name>.bmp, so there is nothing to discover by scanning.
   * `name` is both the label and the artwork's basename; `path` must match
   * the destination in tools/create_disk.sh. */
  static const struct program desktop_candidates[] = {
      {"DOOM", "usr/bin/doom.elf"},
      {"shelf", "system/bin/sh.elf"},
  };
  const int candidate_count =
      (int)(sizeof(desktop_candidates) / sizeof(desktop_candidates[0]));

  desktop_icon_count = 0;
  for (int c = 0; c < candidate_count && desktop_icon_count < MAX_ICONS; c++) {
    /* Skip anything not actually on the volume. DOOM ships only when it was
     * built, so a hardcoded icon for it would otherwise sit on the desktop
     * doing nothing but fail to spawn when clicked. */
    struct stat_user st;
    if (stat_raw(desktop_candidates[c].path, &st) != 0) {
      printf("winman: desktop icon %s skipped, %s not present\n",
             desktop_candidates[c].name, desktop_candidates[c].path);
      continue;
    }

    struct desktop_icon *icon = &desktop_icons[desktop_icon_count++];
    icon->program = desktop_candidates[c];
    icon->x = 20;
    icon->y = 20 + (desktop_icon_count - 1) * 80;
    icon->w = 32;
    icon->h = 32;
  }

  for (int i = 0; i < desktop_icon_count; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/%s.bmp",
             desktop_icons[i].program.name);

    if (bmp_load(path, &desktop_icons[i].icon) == 0) {
      desktop_icons[i].loaded = 1;
    } else {
      printf("winman: icon %s not found, using fallback\n",
             desktop_icons[i].program.name);
      desktop_icons[i].loaded = 0;
    }
  }
}

/* Bounded strcpy into a fixed field; truncates rather than overflowing. */
static void copy_field(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (cap == 0)
    return;
  while (src && src[i] && i + 1 < cap) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

static int ascii_lower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Case-insensitive compare — the volume is FAT, so DOOM.ELF and doom.elf are
 * the same file and must not both end up in the menu. */
static int same_name_ci(const char *a, const char *b) {
  while (*a && *b) {
    if (ascii_lower(*a) != ascii_lower(*b))
      return 0;
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

static int start_menu_has_path(const char *path) {
  for (int i = 0; i < start_menu_count; i++) {
    if (same_name_ci(start_menu_programs[i].path, path))
      return 1;
  }
  return 0;
}

static void start_menu_add(const char *name, const char *path) {
  if (start_menu_count >= START_MENU_MAX || start_menu_has_path(path))
    return;
  struct start_entry *e = &start_menu_programs[start_menu_count++];
  copy_field(e->name, sizeof(e->name), name);
  copy_field(e->path, sizeof(e->path), path);
}

/* How many items fit between the top of the screen and the taskbar. The menu
 * is drawn upward from the taskbar, so without this an over-full directory
 * would place menu_y above 0 and the entries would be clipped off-screen
 * with no way to reach them. */
static int start_menu_capacity(void) {
  int usable = fb_h - TASKBAR_PX - START_MENU_PAD * 2;
  int fits = usable > 0 ? usable / START_MENU_ITEM_H : 0;
  if (fits > START_MENU_MAX)
    fits = START_MENU_MAX;
  if (fits < START_MENU_DEFAULT_COUNT)
    fits = START_MENU_DEFAULT_COUNT; /* pinned entries always get a slot */
  return fits;
}

/* Append every *.elf in `dir` that is not already listed. */
static void start_menu_scan_dir(const char *dir, int capacity) {
  char buf[512];
  unsigned index = 0;
  long n;

  while (start_menu_count < capacity &&
         (n = readdir_path(dir, &index, buf, sizeof(buf))) > 0) {
    /* Packed NUL-terminated names; directories come back with a trailing
     * '/' and are skipped. */
    for (long i = 0; i < n && start_menu_count < capacity; i++) {
      if (buf[i] == 0)
        continue;
      const char *entry = &buf[i];
      size_t len = strlen(entry);
      i += (long)len;

      if (len == 0 || entry[len - 1] == '/')
        continue;

      /* Only the ELF executables — the .exe PE variants are the same
       * programs under another name and would double up the menu. */
      if (len < 5 || !same_name_ci(entry + len - 4, ".elf"))
        continue;

      char path[START_MENU_PATH_MAX];
      copy_field(path, sizeof(path), dir);
      size_t at = strlen(path);
      copy_field(path + at, sizeof(path) - at, "/");
      at = strlen(path);
      copy_field(path + at, sizeof(path) - at, entry);

      /* Label is the filename without the extension. */
      char label[START_MENU_NAME_MAX];
      copy_field(label, sizeof(label), entry);
      size_t label_len = strlen(label);
      if (label_len > 4)
        label[label_len - 4] = 0;

      start_menu_add(label, path);
    }
  }
}

/* Pinned entries first, then each executable directory in order. A missing
 * directory just yields nothing — readdir_path returns <= 0 and the scan
 * moves on, so /usr/local/bin being empty or absent is not an error. */
static void build_start_menu_entries(void) {
  start_menu_count = 0;
  for (int i = 0; i < START_MENU_DEFAULT_COUNT; i++)
    start_menu_add(start_menu_defaults[i].name, start_menu_defaults[i].path);

  int capacity = start_menu_capacity();
  for (int d = 0; d < START_MENU_SCAN_DIR_COUNT; d++)
    start_menu_scan_dir(start_menu_scan_dirs[d], capacity);

  printf("winman: start menu %d entries (%d pinned)\n", start_menu_count,
         START_MENU_DEFAULT_COUNT);
}

static void z_remove(int handle) {
  int j = 0;
  for (int i = 0; i < z_count; i++) {
    if (z_order[i] == handle)
      continue;
    z_order[j++] = z_order[i];
  }
  z_count = j;
}

static void z_bring_to_front(int handle) {
  z_remove(handle);
  if (z_count >= MAX_Z)
    return;
  for (int i = z_count; i > 0; i--)
    z_order[i] = z_order[i - 1];
  z_order[0] = handle;
  z_count++;
}

static void z_send_to_back(int handle) {
  z_remove(handle);
  if (z_count >= MAX_Z)
    return;

  z_order[z_count] = handle;
  z_count++;
}

static int outer_w(const struct window *w) {
  return w->client_w + 2 * BORDER_PX;
}
static int outer_h(const struct window *w) {
  return w->client_h + TITLEBAR_PX + BORDER_PX + w->status_h;
}

/* Pre-expanded glyph row for the console (CONSOLE_FG/CONSOLE_BG are
 * constants): byte -> 8 pixels. Built once in con_alloc. 8 KiB BSS.
 * Per-row glyph render becomes a single memcpy(32 B) instead of 8 per-pixel
 * conditional writes — the dominant cost when winman drains the TTY ring. */
static uint32_t con_glyph_lut[256][FONT_GLYPH_W];

static char trunc_titles[MAX_WINDOWS][TASKBAR_BTN_W + 1];

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
  if (n == 0)
    return;

  /* Force 8-byte alignment if currently only 4-byte aligned */
  if ((uintptr_t)dst & 4) {
    *dst++ = color;
    n--;
  }

  if (n >= 2) {
    uint64_t v = ((uint64_t)color << 32) | color;
    uint64_t *p = (uint64_t *)dst;
    size_t pairs = n >> 1;
    for (size_t i = 0; i < pairs; i++)
      p[i] = v;
    dst += pairs * 2;
    n &= 1;
  }

  if (n) {
    *dst = color;
  }
}

/* Union the new rect into the existing dirty bounding box. */
static void mark_dirty(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fb_w)
    w = fb_w - x;
  if (y + h > fb_h)
    h = fb_h - y;
  if (w <= 0 || h <= 0)
    return;

  if (!desktop_dirty) {
    dirty_x = x;
    dirty_y = y;
    dirty_w = w;
    dirty_h = h;
  } else {
    int x2 = dirty_x + dirty_w;
    int y2 = dirty_y + dirty_h;
    if (x < dirty_x)
      dirty_x = x;
    if (y < dirty_y)
      dirty_y = y;
    if (x + w > x2)
      x2 = x + w;
    if (y + h > y2)
      y2 = y + h;
    dirty_w = x2 - dirty_x;
    dirty_h = y2 - dirty_y;
  }
  desktop_dirty = 1;
}

/* Missing artwork falls back to the built-in mask. */
static struct bmp_image tb_start_icon;
static int tb_start_icon_loaded = 0;
static void tb_load_start_icon(void) {
  if (tb_start_icon_loaded)
    return;

  if (bmp_load(TB_START_ICON_PATH, &tb_start_icon) != 0) {
    printf("winman: %s unavailable, using built-in start icon\n",
           TB_START_ICON_PATH);
    return;
  }

  if (tb_start_icon.width <= 0 || tb_start_icon.height <= 0 ||
      tb_start_icon.width > TB_START_ICON_MAX_DIM ||
      tb_start_icon.height > TB_START_ICON_MAX_DIM) {
    printf("winman: %s has invalid start icon dimensions %dx%d, using "
           "built-in start icon\n",
           TB_START_ICON_PATH, tb_start_icon.width, tb_start_icon.height);
    bmp_free(&tb_start_icon);
    return;
  }

  tb_start_icon_loaded = 1;
  printf("winman: start icon %dx%d from %s\n", tb_start_icon.width,
         tb_start_icon.height, TB_START_ICON_PATH);
}

static struct ttf_font con_font;
static int con_ttf_ready = 0;
static int con_ttf_cell_w = 9;
static int con_ttf_cell_h = 18;
static int con_ttf_ascent = 12;
static int con_ttf_descent = -3;

static int con_cell_w_for_scale(int scale) {
  if (scale < 1)
    scale = 1;
  return (con_ttf_ready ? con_ttf_cell_w : FONT_GLYPH_W) * scale;
}

static int con_cell_h_for_scale(int scale) {
  if (scale < 1)
    scale = 1;
  return (con_ttf_ready ? con_ttf_cell_h : FONT_GLYPH_H) * scale;
}

static int con_cell_w(void) { return con_cell_w_for_scale(con.scale); }
static int con_cell_h(void) { return con_cell_h_for_scale(con.scale); }

static int con_font_px(void) {
  int scale = con.scale < 1 ? 1 : con.scale;
  return CON_TTF_PX * scale;
}

static void con_try_load_ttf(void) {
  if (con_ttf_ready)
    return;

  static const char *paths[] = {
      CON_TTF_PATH,
      "/system/fonts/SansDisplayVariable.ttf",
      "system/fonts/sansdisplayvariable.ttf",
      "system/fonts/SansDisplayVariable.ttf",
      "/system/fonts/sansdisplaystatic.ttf",
      "/system/fonts/SansDisplayStatic.ttf",
      "system/fonts/sansdisplaystatic.ttf",
      "system/fonts/SansDisplayStatic.ttf",
      0,
  };

  const char *loaded_path = 0;
  int rc = -1;
  for (int i = 0; paths[i]; i++) {
    rc = ttf_load(&con_font, paths[i]);
    if (rc == 0) {
      loaded_path = paths[i];
      break;
    }
  }

  if (!loaded_path) {
    printf("winman: TTF console unavailable rc=%d, using font8x8\n", rc);
    return;
  }

  /* Cell is sized off the digit advance; glyphs wider than it are condensed
   * by ttf_draw_glyph_cell rather than clipped. Sizing off the widest glyph
   * instead is what left a gap the size of a space after every character. */
  con_ttf_cell_w = ttf_cell_width(&con_font, CON_TTF_PX);
  if (con_ttf_cell_w < 4)
    con_ttf_cell_w = FONT_GLYPH_W;
  if (con_ttf_cell_w > 20)
    con_ttf_cell_w = 20;
  con_ttf_cell_h = CON_TTF_PX + 3;

  ttf_vmetrics(&con_font, CON_TTF_PX, &con_ttf_ascent, &con_ttf_descent, 0);
  if (con_ttf_ascent <= 0)
    con_ttf_ascent = (CON_TTF_PX * 3) / 4;

  con_ttf_ready = 1;
  printf("winman: TTF console %s cell=%dx%d px=%d asc=%d desc=%d\n",
         loaded_path, con_ttf_cell_w, con_ttf_cell_h, CON_TTF_PX,
         con_ttf_ascent, con_ttf_descent);
}

static int win_get_rect(int handle, int *x, int *y, int *cw, int *ch) {
  if (handle == HANDLE_CONSOLE) {
    if (!con.win.in_use)
      return 0;
    *x = con.win.x;
    *y = con.win.y;
    *cw = con.win.client_w;
    *ch = con.win.client_h;
    return 1;
  }
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle) {
      *x = windows[i].x;
      *y = windows[i].y;
      *cw = windows[i].client_w;
      *ch = windows[i].client_h;
      return 1;
    }
  }
  return 0;
}

static void win_set_pos(int handle, int x, int y) {
  if (handle == HANDLE_CONSOLE) {
    con.win.x = x;
    con.win.y = y;
    return;
  }
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle) {
      windows[i].x = x;
      windows[i].y = y;
      return;
    }
  }
}

static int outer_w_dims(int cw) { return cw + 2 * BORDER_PX; }

/* Outer height from a client height plus that window's status strip. The
 * status height is an explicit parameter rather than looked up inside,
 * because most callers already hold the window and the ones that don't must
 * be made to say which window they mean — a frame height that silently
 * omits the strip leaves it unpainted or untestable. */
static int outer_h_dims(int ch, int status_h) {
  return ch + TITLEBAR_PX + BORDER_PX + status_h;
}

/* Status height for a handle, 0 if it has no window or no strip. For the
 * call sites that only carry a handle. */
static int status_h_of(int handle) {
  struct window *w = find_handle(handle);
  return w ? w->status_h : 0;
}

/* True iff (mx,my) lies inside titlebar button `idx_from_right` for a window
 * whose outer rect is (win_x, win_y, outer_w, _). Used by hit_test_at to
 * carve close/min/max regions out of HIT_TITLEBAR before returning. */
static int in_titlebar_btn(int win_x, int win_y, int outer_w,
                           int idx_from_right, int mx, int my) {
  int bx, by, bw, bh;
  titlebar_btn_rect(win_x, win_y, outer_w, idx_from_right, &bx, &by, &bw, &bh);
  return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

/* Enumerate the windows that should appear on the taskbar in stable order:
 * the console first (handle 0), then in-use client windows in their array
 * slot order. Order matches z-order today since neither has explicit raising.
 */
struct tb_entry {
  int handle;
  const char *title;
};

static int build_taskbar_entries(struct tb_entry *out, int max) {
  int n = 0;
  if (con.win.in_use && n < max) {
    out[n].handle = HANDLE_CONSOLE;
    out[n].title = con.win.title;
    n++;
  }

  int max_chars = (TASKBAR_BTN_W - 8) / FONT_GLYPH_W;

  for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
    if (!windows[i].in_use)
      continue;
    out[n].handle = windows[i].handle;
    size_t title_len = strlen(windows[i].title);

    if ((int)title_len <= max_chars) {
      out[n].title = windows[i].title;
    } else {
      char *trunc = trunc_titles[i];
      strncpy(trunc, windows[i].title, max_chars - 3);
      trunc[max_chars - 3] = '\0';
      strcat(trunc, "...");
      out[n].title = trunc;
    }
    n++;
  }
  return n;
}

static int taskbar_y(void) { return fb_h - TASKBAR_PX; }

/* Rightmost x a taskbar button may occupy. Without this the button strip
 * grows under the clock and paints over it. */
static int taskbar_btn_limit(void) {
  return fb_w - CLOCK_W - CLOCK_PAD_R - TASKBAR_BTN_GAP;
}

/* Returns 1 + writes *out_handle when the click landed on a taskbar button,
 * 0 otherwise (including clicks on the taskbar strip background). Caller
 * should still treat strip clicks as "ate the input" — the strip is opaque
 * and never belongs to a client. */
static int hit_taskbar(int mx, int my, int *out_handle) {
  int y = taskbar_y();
  if (my < y || my >= y + TASKBAR_PX)
    return 0;

  struct tb_entry ents[1 + MAX_WINDOWS];
  int n = build_taskbar_entries(ents, (int)(sizeof(ents) / sizeof(ents[0])));

  int bx = TASKBAR_START_W + TASKBAR_BTN_GAP;
  int by = y;
  int bh = TASKBAR_PX;

  for (int i = 0; i < n; i++) {
    if (bx + TASKBAR_BTN_W > taskbar_btn_limit())
      break;
    if (mx >= bx && mx < bx + TASKBAR_BTN_W && my >= by && my < by + bh) {
      *out_handle = ents[i].handle;
      return 1;
    }
    bx += TASKBAR_BTN_W + TASKBAR_BTN_GAP;
  }
  return 0;
}

static int in_start_menu(int menu_x, int menu_y, int mx, int my) {
  if (!start_menu_open)
    return 0;
  int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
  return mx >= menu_x && mx < menu_x + START_MENU_W && my >= menu_y &&
         my < menu_y + menu_h;
}

/* Classify a screen-space hit by walking the z-order topmost-first. The
 * console is just another z-stack entry now; whichever handle is at
 * z_order[0] wins ties at the same pixel. */
static int hit_test_at(int mx, int my, int *out_handle) {
  if (start_menu_open) {
    int menu_x = 0;
    int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
    int menu_y = taskbar_y() - menu_h;
    if (in_start_menu(menu_x, menu_y, mx, my)) {
      return HIT_START_MENU;
    }
  }

  if (my >= taskbar_y()) {
    if (mx >= 0 && mx < TASKBAR_START_W) {
      return HIT_START_BTN;
    }
    int tb_handle = 0;
    if (hit_taskbar(mx, my, &tb_handle)) {
      *out_handle = tb_handle;
      return HIT_TASKBAR_BTN;
    }
    return HIT_NONE;
  }

  for (int i = 0; i < z_count; i++) {
    int h = z_order[i];
    if (is_minimized(h))
      continue;
    int x, y, cw, ch;
    if (!win_get_rect(h, &x, &y, &cw, &ch))
      continue;
    int ow = outer_w_dims(cw);
    int oh = outer_h_dims(ch, status_h_of(h));
    if (mx < x || mx >= x + ow || my < y || my >= y + oh)
      continue;
    *out_handle = h;
    /* Grip beats titlebar when they overlap at the seam. */
    if (mx >= x + ow - RESIZE_GRIP && my >= y + oh - RESIZE_GRIP)
      return HIT_GRIP;
    if (my < y + TITLEBAR_PX) {
      if (in_titlebar_btn(x, y, ow, 0, mx, my))
        return HIT_BTN_CLOSE;
      if (in_titlebar_btn(x, y, ow, 1, mx, my))
        return HIT_BTN_MAX;
      if (in_titlebar_btn(x, y, ow, 2, mx, my))
        return HIT_BTN_MIN;
      return HIT_TITLEBAR;
    }
    /* Below the client area is the status strip, not the client. Returning
     * HIT_CLIENT here would forward the click at a y past the bottom of the
     * client's surface. The grip check above already claimed the corner. */
    if (status_h_of(h) > 0 && my >= y + TITLEBAR_PX + ch)
      return HIT_STATUSBAR;
    return HIT_CLIENT;
  }
  return HIT_NONE;
}

static void clamp_to_desktop(int *x, int *y, int cw, int ch) {
  int ow = outer_w_dims(cw);
  (void)ch;
  /* Keep at least the title bar reachable for re-drag. Bottom limit also
   * subtracts the taskbar so windows can't hide their title strip behind it. */
  if (*x + ow < TITLEBAR_PX)
    *x = TITLEBAR_PX - ow;
  if (*y < 0)
    *y = 0;
  if (*x > fb_w - TITLEBAR_PX)
    *x = fb_w - TITLEBAR_PX;
  if (*y > fb_h - TITLEBAR_PX - TASKBAR_PX)
    *y = fb_h - TITLEBAR_PX - TASKBAR_PX;
}

/* Console resize: reallocate surface + backing buffer at new dims and copy
 * the still-visible region of the old surface into the new one so existing
 * glyphs survive. Cursor is clamped to the new grid. */
static void console_resize(int new_cw, int new_ch) {
  if (!con.win.in_use)
    return;
  /* Snap to glyph grid so cells line up without trailing fractional row. */
  int cell_w = con_cell_w();
  int cell_h = con_cell_h();
  new_cw = (new_cw / cell_w) * cell_w;
  new_ch = (new_ch / cell_h) * cell_h;
  if (new_cw < MIN_CLIENT_W)
    new_cw = MIN_CLIENT_W;
  if (new_ch < MIN_CLIENT_H)
    new_ch = MIN_CLIENT_H;
  if (new_cw == con.win.client_w && new_ch == con.win.client_h)
    return;

  size_t pixel_bytes = (size_t)new_cw * (size_t)new_ch * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *new_raw = 0;
  uint32_t *new_surf = (uint32_t *)aligned_page_alloc(pages, &new_raw);
  if (!new_surf)
    return;
  fill_dwords(new_surf, (size_t)new_cw * (size_t)new_ch, CONSOLE_BG);

  void *new_back_raw = 0;
  uint32_t *new_back = (uint32_t *)aligned_page_alloc(pages, &new_back_raw);
  if (!new_back) {
    free(new_raw);
    return;
  }
  fill_dwords(new_back, (size_t)new_cw * (size_t)new_ch, CONSOLE_BG);
  int new_cols = new_cw / cell_w;
  int new_rows = new_ch / cell_h;
  char *new_cells = (char *)malloc((size_t)new_cols * (size_t)new_rows);
  char *new_saved = (char *)malloc((size_t)new_cols * (size_t)new_rows);
  if (!new_cells || !new_saved) {
    if (new_cells)
      free(new_cells);
    if (new_saved)
      free(new_saved);
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
      memcpy(new_cells + y * new_cols, con.cells + y * con.cols,
             (size_t)copy_cols);
    }
  }

  if (con.win.surface_raw)
    free(con.win.surface_raw);
  if (con_backing_raw)
    free(con_backing_raw);
  if (con.cells)
    free(con.cells);
  if (con_saved_cells)
    free(con_saved_cells);

  con.win.surface = new_surf;
  con.win.surface_raw = new_raw;
  con.cells = new_cells;
  con_backing = new_back;
  con_backing_raw = new_back_raw;
  con_saved_cells = new_saved;
  con.win.client_w = new_cw;
  con.win.client_h = new_ch;
  con.cols = new_cols;
  con.rows = new_rows;
  if (con.cx >= con.cols)
    con.cx = con.cols - 1;
  if (con.cy >= con.rows)
    con.cy = con.rows - 1;
  con_saved_valid = 0;
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
  clamp_client_size(&new_cw, &new_ch);

  if (handle == HANDLE_CONSOLE) {
    console_resize(new_cw, new_ch);
    return;
  }

  struct window *w = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle) {
      w = &windows[i];
      break;
    }
  }
  if (!w) {
    printf("winman: client_window_resize: no window found for handle=%d\n",
           handle);
    return;
  }
  if (new_cw == w->client_w && new_ch == w->client_h)
    return;

  size_t pixel_bytes = (size_t)new_cw * (size_t)new_ch * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *raw = 0;
  uint32_t *surface = (uint32_t *)aligned_page_alloc(pages, &raw);
  if (!surface)
    return;

  /* Preserve the upper-left intersection of the old surface — same idea
   * as console_resize. The client can't paint until it receives the
   * resize notify, so racing it for these reads is acceptable. */
  if (w->surface) {
    int copy_w = w->client_w < new_cw ? w->client_w : new_cw;
    int copy_h = w->client_h < new_ch ? w->client_h : new_ch;
    for (int y = 0; y < copy_h; y++) {
      memcpy(surface + (size_t)y * (size_t)new_cw,
             w->surface + (size_t)y * (size_t)w->client_w, (size_t)copy_w * 4);
    }
  }

  uint64_t client_va = 0;
  if (shmem_share(w->owner_pid, (uint64_t)surface, (long)pages, &client_va) !=
      0) {
    free(raw);
    return;
  }

  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));

  uint64_t old_client_va = w->client_va;
  int old_n_pages = w->n_pages;
  void *old_raw = w->surface_raw;

  w->surface = surface;
  w->surface_raw = raw;
  w->n_pages = (int)pages;
  w->client_va = client_va;
  w->client_w = new_cw;
  w->client_h = new_ch;

  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));

  struct ipc_msg note;
  memset(&note, 0, (sizeof(note)));
  note.type = IPC_WM_RESIZE_NOTIFY;
  note.a = handle;
  note.b = new_cw;
  note.c = new_ch;
  note.va = client_va;
  note.pitch = (uint32_t)(new_cw * 4);
  ipc_send(w->owner_pid, &note);

  // free old client
  if (old_client_va && old_n_pages > 0) {
    // unmap old client range w->owner_pid, old_client_va, old_n_pages
    shmem_unshare(w->owner_pid, old_client_va, old_n_pages);
  };

  if (old_raw)
    free(old_raw);
}

static void *aligned_page_alloc(size_t npages, void **out_raw) {
  size_t need = npages * 4096 + 4095;
  void *raw = malloc(need);
  if (!raw)
    return 0;
  memset(raw, 0, need);
  uintptr_t v = ((uintptr_t)raw + 4095) & ~(uintptr_t)4095;
  if (out_raw)
    *out_raw = raw;
  return (void *)v;
}

/* Grow geometrically and retain the allocation across host resizes. Width-only
 * changes normally need no growth because virtio keeps a stable row pitch. */
static int backbuffer_reserve(size_t required) {
  if (required <= fb_capacity)
    return 0;

  size_t pages = fb_capacity / 4096;
  if (pages == 0)
    pages = 1;
  size_t required_pages = (required + 4095) / 4096;
  while (pages < required_pages)
    pages *= 2;

  size_t capacity = pages * 4096;
  uint32_t *new_fb = (uint32_t *)mmap(0, capacity, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (new_fb == MAP_FAILED)
    return -1;

  if (fb_registered) {
    fb_unregister();
    fb_registered = 0;
  }
  if (fb && fb_capacity)
    munmap(fb, fb_capacity);
  fb = new_fb;
  fb_capacity = capacity;
  return 0;
}

/* Registration snapshots the backbuffer's physical pages once. Presents can
 * then dispatch directly to APs without walking every user page each frame. */
static int backbuffer_register(void) {
  if (!fb || fb_stride <= 0)
    return -1;
  if (fb_register(fb, (uint32_t)fb_stride * 4U) != 0) {
    /* A failed replacement may leave an older registration active. Drop it so
     * later reallocations cannot be rejected as overlapping a pinned range. */
    fb_unregister();
    fb_registered = 0;
    return -1;
  }
  fb_registered = 1;
  return 0;
}

static inline uint32_t *fb_pix(int x, int y) {
  return fb + (size_t)y * (size_t)fb_stride + (size_t)x;
}

/* Compose clip: the region present_dirty() is about to copy out to the
 * hardware framebuffer. Every compositor primitive must honour this clip:
 * the backbuffer persists between frames, so an out-of-clip write is not
 * harmless. Cursor and drag repairs may expose it during a later present. */
static int clip_x, clip_y, clip_w, clip_h;

static void clip_set(int x, int y, int w, int h) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fb_w)
    w = fb_w - x;
  if (y + h > fb_h)
    h = fb_h - y;
  clip_x = x;
  clip_y = y;
  clip_w = w < 0 ? 0 : w;
  clip_h = h < 0 ? 0 : h;
}

/* True when [x,x+w) x [y,y+h) has any pixel inside the clip. */
static int clip_hits(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0 || clip_w <= 0 || clip_h <= 0)
    return 0;
  return x < clip_x + clip_w && x + w > clip_x && y < clip_y + clip_h &&
         y + h > clip_y;
}

static int clip_contains_point(int x, int y) {
  return clip_w > 0 && clip_h > 0 && x >= clip_x && y >= clip_y &&
         x < clip_x + clip_w && y < clip_y + clip_h;
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fb_w)
    w = fb_w - x;
  if (y + h > fb_h)
    h = fb_h - y;
  if (x < clip_x) {
    w -= clip_x - x;
    x = clip_x;
  }
  if (y < clip_y) {
    h -= clip_y - y;
    y = clip_y;
  }
  if (x + w > clip_x + clip_w)
    w = clip_x + clip_w - x;
  if (y + h > clip_y + clip_h)
    h = clip_y + clip_h - y;
  if (w <= 0 || h <= 0)
    return;
  uint32_t *row = fb_pix(x, y);
  for (int yy = 0; yy < h; yy++) {
    fill_dwords(row, (size_t)w, color);
    row += fb_stride;
  }
}

static void draw_glyph_fb(int x, int y, char c, uint32_t fg, uint32_t bg) {
  if (c < FONT_FIRST || c > FONT_LAST)
    c = ' ';
  /* Single-shot intersection with the screen and active compose clip keeps
   * the inner glyph loop free of per-pixel bounds checks. */
  int x0 = x > clip_x ? x : clip_x;
  int y0 = y > clip_y ? y : clip_y;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  int x1 = x + FONT_GLYPH_W;
  int y1 = y + FONT_GLYPH_H;
  if (x1 > fb_w)
    x1 = fb_w;
  if (y1 > fb_h)
    y1 = fb_h;
  if (x1 > clip_x + clip_w)
    x1 = clip_x + clip_w;
  if (y1 > clip_y + clip_h)
    y1 = clip_y + clip_h;
  if (x0 >= x1 || y0 >= y1)
    return;
  int col_first = x0 - x;
  int col_last = x1 - x;
  int row_first = y0 - y;
  int row_last = y1 - y;
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

static void draw_text_fb(int x, int y, const char *s, int max_w, uint32_t fg,
                         uint32_t bg) {
  int drawn = 0;
  while (*s && drawn + FONT_GLYPH_W <= max_w) {
    draw_glyph_fb(x + drawn, y, *s, fg, bg);
    s++;
    drawn += FONT_GLYPH_W;
  }
}

/* Compute screen-space rect of titlebar button `idx_from_right` (0 = closest
 * to the corner). Used by both draw_chrome and the input pump's hit_test
 * so click rects exactly match what was rendered. */
static void titlebar_btn_rect(int win_x, int win_y, int outer_w,
                              int idx_from_right, int *bx, int *by, int *bw,
                              int *bh) {
  *bw = TB_BTN_SIZE;
  *bh = TB_BTN_SIZE;
  *bx = win_x + outer_w - BORDER_PX - TB_BTN_PAD_R -
        (idx_from_right + 1) * TB_BTN_SIZE - idx_from_right * TB_BTN_GAP;
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
      if (!mask[r][c])
        continue;
      int px = x + c;
      int py = y + r;
      if (px < 0 || px >= fb_w || py < 0 || py >= fb_h ||
          !clip_contains_point(px, py))
        continue;
      *fb_pix(px, py) = fg;
    }
  }
}

/* Stamp a TB_BTN_SIZE square button at (x,y). bg fills the rect; fg appears
 * only where the mask is 1. Mirrors the cursor's mask-overlay rendering. */
static void
draw_button_mask_large(int x, int y,
                       const uint8_t mask[TASKBAR_START_W][TASKBAR_START_W],
                       uint32_t fg, uint32_t bg) {
  fb_fill_rect(x, y, TASKBAR_START_W, TASKBAR_START_W, bg);
  for (int r = 0; r < TASKBAR_START_W; r++) {
    for (int c = 0; c < TASKBAR_START_W; c++) {
      if (!mask[r][c])
        continue;
      int px = x + c;
      int py = y + r;
      if (px < 0 || px >= fb_w || py < 0 || py >= fb_h ||
          !clip_contains_point(px, py))
        continue;
      *fb_pix(px, py) = fg;
    }
  }
}

static void draw_chrome(const struct window *w, int focused) {
  int ow = outer_w(w);
  int oh = outer_h(w);
  fb_fill_rect(w->x, w->y, ow, oh, CHROME_BG);
  int tb_x = w->x + BORDER_PX;
  int tb_y = w->y;
  int tb_w = ow - 2 * BORDER_PX;
  int tb_h = TITLEBAR_PX - BORDER_PX;
  uint32_t bg = focused ? TITLEBAR_FG : TITLEBAR_BG;
  fb_fill_rect(tb_x, tb_y, tb_w, tb_h, bg);

  /* Reserve space at the right for close + max + min buttons. Title text
   * gets clamped so it never overlaps the buttons. Order from right to
   * left: close (idx 0), max (1), min (2). */
  const int n_btns = 3;
  int btn_strip_w =
      n_btns * TB_BTN_SIZE + (n_btns - 1) * TB_BTN_GAP + TB_BTN_PAD_R;

  int text_x = tb_x + 4;
  int text_y = tb_y + (tb_h - FONT_GLYPH_H) / 2;
  int avail = tb_w - 8 - btn_strip_w;
  if (avail > 0) {
    draw_text_fb(text_x, text_y, w->title, avail, CHROME_TEXT, bg);
  }

  int bx, by, bw_, bh_;
  titlebar_btn_rect(w->x, w->y, ow, 0, &bx, &by, &bw_, &bh_);
  draw_button_mask(bx, by, fallback_btn_close_mask, TB_BTN_FG, TB_BTN_BG);
  titlebar_btn_rect(w->x, w->y, ow, 1, &bx, &by, &bw_, &bh_);
  draw_button_mask(bx, by, fallback_btn_maximise_mask, TB_BTN_FG, TB_BTN_BG);
  titlebar_btn_rect(w->x, w->y, ow, 2, &bx, &by, &bw_, &bh_);
  draw_button_mask(bx, by, fallback_btn_hide_mask, TB_BTN_FG, TB_BTN_BG);

  /* Status strip, below the client area and inside the border. */
  if (w->status_h > 0) {
    int sx = w->x + BORDER_PX;
    int sy = w->y + TITLEBAR_PX + w->client_h;
    int sw = ow - 2 * BORDER_PX;
    fb_fill_rect(sx, sy, sw, w->status_h, STATUSBAR_BG);
    /* 1px top rule so the strip reads as chrome rather than as more client
     * area, which matters most when the client is also light-coloured. */
    fb_fill_rect(sx, sy, sw, 1, TITLEBAR_BG);

    int ty = sy + (w->status_h - FONT_GLYPH_H) / 2;
    int avail = sw - 2 * STATUSBAR_PAD_X;
    if (avail > 0 && w->status[0])
      draw_text_fb(sx + STATUSBAR_PAD_X, ty, w->status, avail, STATUSBAR_FG,
                   STATUSBAR_BG);
  }
}

static void blit_surface(const struct window *w) {
  int dst_x = w->x + BORDER_PX;
  int dst_y = w->y + TITLEBAR_PX;
  int cw = w->client_w;
  int ch = w->client_h;

  /* Clamp to the clip box, not just the screen: rows and columns outside
   * the region about to be presented would be copied for nothing. */
  int x0 = dst_x < clip_x ? clip_x : dst_x;
  int y0 = dst_y < clip_y ? clip_y : dst_y;
  int x1 = (dst_x + cw) > clip_x + clip_w ? clip_x + clip_w : dst_x + cw;
  int y1 = (dst_y + ch) > clip_y + clip_h ? clip_y + clip_h : dst_y + ch;
  if (x0 >= x1 || y0 >= y1)
    return;

  int src_x = x0 - dst_x;
  int src_y = y0 - dst_y;
  int span_w = x1 - x0;

  for (int yy = 0; yy < y1 - y0; yy++) {
    uint32_t *src =
        w->surface + (size_t)(src_y + yy) * (size_t)cw + (size_t)src_x;
    uint32_t *dst = fb_pix(x0, y0 + yy);
    memcpy(dst, src, (size_t)span_w * 4);
  }
}

static void blit_icon(int dst_x, int dst_y, int dst_w, int dst_h, int src_w,
                      int src_h, uint32_t *pixels);

/* Scale available artwork inside the padded button. */
static void draw_start_button(int y) {
  if (!tb_start_icon_loaded) {
    draw_button_mask_large(0, y, fallback_taskbar_start_mask, PRINT_COLOR_GREEN,
                           TASKBAR_BG);
    return;
  }

  /* Clear behind transparent icon pixels. */
  fb_fill_rect(0, y, TASKBAR_START_W, TASKBAR_PX, TASKBAR_BG);

  int pad = TB_START_ICON_PAD;
  if (2 * pad >= TASKBAR_START_W || 2 * pad >= TASKBAR_PX)
    pad = 0;

  blit_icon(pad, y + pad, TASKBAR_START_W - 2 * pad, TASKBAR_PX - 2 * pad,
            tb_start_icon.width, tb_start_icon.height, tb_start_icon.pixels);
}


/* Rendered clock text, refreshed by clock_tick(). Kept as formatted strings
 * rather than re-derived at draw time so a repaint triggered by something
 * else cannot show a different minute than the one that was damaged. */
static char clock_time_text[16];
static char clock_date_text[16];

static void clock_format(void) {
  struct calendar_time now;
  time_to_calendar(time(0), &now);

  /* 12-hour with AM/PM, matching the taskbar it is modelled on. Hour 0 is
   * 12 AM and hour 12 is 12 PM — the modulo alone gets both wrong. */
  int hour12 = now.hour % 12;
  if (hour12 == 0)
    hour12 = 12;

  snprintf(clock_time_text, sizeof(clock_time_text), "%d:%02d %s", hour12,
           now.minute, now.hour < 12 ? "AM" : "PM");
  snprintf(clock_date_text, sizeof(clock_date_text), "%02d/%02d/%04d", now.day,
           now.month, now.year);
}

static int clock_rect(int *cx, int *cy, int *cw, int *ch) {
  *cw = CLOCK_W;
  *ch = TASKBAR_PX;
  *cx = fb_w - CLOCK_W - CLOCK_PAD_R;
  *cy = taskbar_y();
  return *cx > TASKBAR_START_W && *cy >= 0;
}

/* Reformat and damage the clock when the displayed text actually changes.
 * Returns 1 if a repaint was requested. */
static int clock_tick(void) {
  char prev_time[sizeof(clock_time_text)];
  char prev_date[sizeof(clock_date_text)];
  memcpy(prev_time, clock_time_text, sizeof(prev_time));
  memcpy(prev_date, clock_date_text, sizeof(prev_date));

  clock_format();

  if (strcmp(prev_time, clock_time_text) == 0 &&
      strcmp(prev_date, clock_date_text) == 0)
    return 0;

  int cx, cy, cw, ch;
  if (!clock_rect(&cx, &cy, &cw, &ch))
    return 0;
  mark_dirty(cx, cy, cw, ch);
  return 1;
}

static void draw_clock(void) {
  int cx, cy, cw, ch;
  if (!clock_rect(&cx, &cy, &cw, &ch))
    return;

  fb_fill_rect(cx, cy, cw, ch, TASKBAR_BG);

  /* Two rows centred as a block in the strip. */
  int block_h = FONT_GLYPH_H * 2 + CLOCK_LINE_GAP;
  int top = cy + (ch - block_h) / 2;

  int time_w = (int)strlen(clock_time_text) * FONT_GLYPH_W;
  int date_w = (int)strlen(clock_date_text) * FONT_GLYPH_W;

  draw_text_fb(cx + (cw - time_w) / 2, top, clock_time_text, cw, CLOCK_FG,
               TASKBAR_BG);
  draw_text_fb(cx + (cw - date_w) / 2, top + FONT_GLYPH_H + CLOCK_LINE_GAP,
               clock_date_text, cw, CLOCK_FG, TASKBAR_BG);
}

static void draw_taskbar(void) {
  int y = taskbar_y();
  if (y < 0)
    return;
  // fill the entire taskbar
  fb_fill_rect(0, y, fb_w, TASKBAR_PX, TASKBAR_BG);

  // draw the start button FIRST at x=0 so it doesn't cover taskbar buttons
  draw_start_button(y);

  // draw divider right after the start button
  fb_fill_rect(TASKBAR_START_W + TASKBAR_BTN_GAP, y, 2, TASKBAR_PX,
               PRINT_COLOR_BLACK);

  struct tb_entry ents[1 + MAX_WINDOWS];
  int n = build_taskbar_entries(ents, (int)(sizeof(ents) / sizeof(ents[0])));

  /* 3. Draw taskbar buttons */
  int bx = TASKBAR_START_W + TASKBAR_BTN_GAP;
  int by = y;
  int bh = TASKBAR_PX;

  for (int i = 0; i < n; i++) {
    if (bx + TASKBAR_BTN_W > taskbar_btn_limit())
      break;
    int focused = ents[i].handle == focused_handle;
    uint32_t bg = focused ? TASKBAR_BTN_BG_FOCUS : TASKBAR_BTN_BG;
    uint32_t fg = focused ? TASKBAR_BTN_TEXT_FOC : TASKBAR_BTN_TEXT;

    fb_fill_rect(bx, by, TASKBAR_BTN_W, bh, bg);
    int tx = bx + 4;
    int ty = by + (bh - FONT_GLYPH_H) / 2;
    draw_text_fb(tx, ty, ents[i].title, TASKBAR_BTN_W - 8, fg, bg);
    bx += TASKBAR_BTN_W + TASKBAR_BTN_GAP;
  }

  draw_clock();
}


static void draw_start_menu(void) {
  if (!start_menu_open)
    return;

  int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
  int mx = 0;
  int my = taskbar_y() - menu_h;

  fb_fill_rect(mx, my, START_MENU_W, menu_h, MENU_BG);

  fb_fill_rect(mx, my, START_MENU_W, 1, PRINT_COLOR_BLACK);
  fb_fill_rect(mx, my + menu_h - 1, START_MENU_W, 1,
               PRINT_COLOR_BLACK);
  fb_fill_rect(mx, my, 1, menu_h, PRINT_COLOR_BLACK);
  fb_fill_rect(mx + START_MENU_W - 1, my, 1, menu_h,
               PRINT_COLOR_BLACK);

  for (int i = 0; i < start_menu_count; i++) {
    int item_y = my + START_MENU_PAD + (i * START_MENU_ITEM_H);
    int item_x = mx + START_MENU_PAD;
    int item_w = START_MENU_W - (START_MENU_PAD * 2);

    uint32_t bg = (i == start_menu_hover) ? MENU_HOVER_BG : MENU_BG;
    uint32_t fg = (i == start_menu_hover) ? MENU_HOVER_FG : MENU_TEXT;

    fb_fill_rect(item_x, item_y, item_w, START_MENU_ITEM_H - 2, bg);
    draw_text_fb(item_x + 4, item_y + (START_MENU_ITEM_H - FONT_GLYPH_H) / 2,
                 start_menu_programs[i].name, item_w - 8, fg, bg);
  }
}

static void con_draw_glyph(int gx, int gy, char c) {
  int cell_w = con_cell_w();
  int cell_h = con_cell_h();
  int px = gx * cell_w;
  int py = gy * cell_h;
  if (px + cell_w > con.win.client_w)
    return;
  if (py + cell_h > con.win.client_h)
    return;

  if (con_ttf_ready) {
    for (int y = 0; y < cell_h; y++) {
      uint32_t *line = con.win.surface +
                       (size_t)(py + y) * (size_t)con.win.client_w + (size_t)px;
      fill_dwords(line, (size_t)cell_w, CONSOLE_BG);
    }

    if ((unsigned char)c < 32 || (unsigned char)c > 126 || c == ' ')
      return;

    struct gfx_surface s;
    gfx_surface_init(&s, con.win.surface, con.win.client_w, con.win.client_h,
                     con.win.client_w);
    struct gfx_rect prev =
        gfx_clip_push(&s, gfx_rect_make(px, py, cell_w, cell_h));

    /* Sit the baseline so the ascender/descender band is centred in the cell
     * instead of guessing at 3/4 of the way down. */
    int scale = con.scale < 1 ? 1 : con.scale;
    int band = (con_ttf_ascent - con_ttf_descent) * scale;
    int baseline = py + (cell_h - band) / 2 + con_ttf_ascent * scale;

    ttf_draw_glyph_cell(&s, &con_font, px, baseline, cell_w, (unsigned char)c,
                        con_font_px(), CONSOLE_FG);
    gfx_clip_set(&s, prev);
    return;
  }

  const uint8_t *glyph;
  static const uint8_t blank[FONT_GLYPH_H] = {0};
  if (c < FONT_FIRST || c > FONT_LAST)
    glyph = blank;
  else
    glyph = font8x8[(int)c - FONT_FIRST];

  if (con.scale == 1) {
    uint32_t *line =
        con.win.surface + (size_t)py * (size_t)con.win.client_w + (size_t)px;
    for (int r = 0; r < FONT_GLYPH_H; r++) {
      memcpy(line, con_glyph_lut[glyph[r]], FONT_GLYPH_W * sizeof(uint32_t));
      line += con.win.client_w;
    }
    return;
  }

  for (int r = 0; r < FONT_GLYPH_H; r++) {
    for (int sy = 0; sy < con.scale; sy++) {
      int y = py + r * con.scale + sy;
      uint32_t *line =
          con.win.surface + (size_t)y * (size_t)con.win.client_w + (size_t)px;
      for (int col = 0; col < FONT_GLYPH_W; col++) {
        uint32_t color = ((glyph[r] >> col) & 1) ? CONSOLE_FG : CONSOLE_BG;
        for (int sx = 0; sx < con.scale; sx++)
          line[col * con.scale + sx] = color;
      }
    }
  }
}

static void con_redraw(void) {
  if (!con.win.in_use || !con.win.surface || !con.cells)
    return;
  fill_dwords(con.win.surface,
              (size_t)con.win.client_w * (size_t)con.win.client_h, CONSOLE_BG);
  for (int y = 0; y < con.rows; y++) {
    for (int x = 0; x < con.cols; x++) {
      char c = con.cells[y * con.cols + x];
      if (c)
        con_draw_glyph(x, y, c);
    }
  }
  mark_dirty(con.win.x, con.win.y, outer_w_dims(con.win.client_w),
             outer_h_dims(con.win.client_h, con.win.status_h));
}

static int con_set_scale(int new_scale) {
  if (!con.win.in_use)
    return -1;
  if (new_scale < CON_SCALE_MIN)
    new_scale = CON_SCALE_MIN;
  if (new_scale > CON_SCALE_MAX)
    new_scale = CON_SCALE_MAX;
  if (new_scale == con.scale)
    return con.scale;

  int new_cols = con.win.client_w / con_cell_w_for_scale(new_scale);
  int new_rows = con.win.client_h / con_cell_h_for_scale(new_scale);
  if (new_cols <= 0 || new_rows <= 0)
    return -1;

  char *new_cells = (char *)malloc((size_t)new_cols * (size_t)new_rows);
  char *new_saved = (char *)malloc((size_t)new_cols * (size_t)new_rows);
  if (!new_cells || !new_saved) {
    if (new_cells)
      free(new_cells);
    if (new_saved)
      free(new_saved);
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
           con.cells + (src_y0 + y) * con.cols, (size_t)copy_cols);
  }

  int new_cx = con.cx < new_cols ? con.cx : new_cols - 1;
  int new_cy = con.cy - src_y0 + dst_y0;
  if (new_cy < 0)
    new_cy = 0;
  if (new_cy >= new_rows)
    new_cy = new_rows - 1;

  free(con.cells);
  if (con_saved_cells)
    free(con_saved_cells);
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
  if (con.rows <= 1)
    return;
  memmove(con.cells, con.cells + con.cols,
          (size_t)(con.rows - 1) * (size_t)con.cols);
  memset(con.cells + (con.rows - 1) * con.cols, 0, (size_t)con.cols);
  con_redraw();
}

static void con_newline(void) {
  con.cx = 0;
  con.cy++;
  if (con.cy >= con.rows) {
    con_scroll();
    con.cy = con.rows - 1;
  }
}

/* Wipe the live console surface back to CONSOLE_BG and home the cursor.
 * Used by both TTY_CTRL_CLEAR and the entry path of TTY_CTRL_PUSH. */
static void con_wipe(void) {
  if (!con.win.in_use || !con.win.surface)
    return;
  fill_dwords(con.win.surface,
              (size_t)con.win.client_w * (size_t)con.win.client_h, CONSOLE_BG);
  if (con.cells)
    memset(con.cells, 0, (size_t)con.cols * (size_t)con.rows);
  con.cx = con.cy = 0;
  mark_dirty(con.win.x, con.win.y, outer_w_dims(con.win.client_w),
             outer_h_dims(con.win.client_h, con.win.status_h));
}

static void con_save(void) {
  if (!con.win.in_use || !con.win.surface || !con_backing)
    return;
  size_t pixels = (size_t)con.win.client_w * (size_t)con.win.client_h;
  memcpy(con_backing, con.win.surface, pixels * 4);
  if (con.cells && con_saved_cells)
    memcpy(con_saved_cells, con.cells, (size_t)con.cols * (size_t)con.rows);
  con_saved_cx = con.cx;
  con_saved_cy = con.cy;
  con_saved_valid = 1;
}

static void con_restore(void) {
  if (!con.win.in_use || !con.win.surface || !con_backing || !con_saved_valid)
    return;
  size_t pixels = (size_t)con.win.client_w * (size_t)con.win.client_h;
  memcpy(con.win.surface, con_backing, pixels * 4);
  if (con.cells && con_saved_cells)
    memcpy(con.cells, con_saved_cells, (size_t)con.cols * (size_t)con.rows);
  con.cx = con_saved_cx;
  con.cy = con_saved_cy;
  con_saved_valid = 0;
  mark_dirty(con.win.x, con.win.y, outer_w_dims(con.win.client_w),
             outer_h_dims(con.win.client_h, con.win.status_h));
}

static void con_putc(char c) {
  if (!con.win.in_use)
    return;

  /* Control characters (newline, scroll, clear, etc.) call con_redraw/con_wipe,
   * which already mark the whole console dirty. We only need to mark the
   * specific cell for standard printable characters. */
  if (c == '\n') {
    con_newline();
    return;
  }
  if (c == '\r') {
    con.cx = 0;
    return;
  }
  if (c == '\b') {
    if (con.cx > 0) {
      con.cx--;
      con.cells[con.cy * con.cols + con.cx] = 0;
      con_draw_glyph(con.cx, con.cy, ' ');

      /* Mark only the erased cell dirty */
      int cell_x = con.win.x + BORDER_PX + con.cx * con_cell_w();
      int cell_y = con.win.y + TITLEBAR_PX + con.cy * con_cell_h();
      mark_dirty(cell_x, cell_y, con_cell_w(), con_cell_h());
    }
    return;
  }
  if (c == '\t') {
    do {
      con_putc(' ');
    } while (con.cx % 8);
    return;
  }
  if (c == TTY_CTRL_CLEAR) {
    con_wipe();
    return;
  }
  if (c == TTY_CTRL_PUSH) {
    con_save();
    con_wipe();
    return;
  }
  if (c == TTY_CTRL_POP) {
    con_restore();
    return;
  }
  if (c == TTY_CTRL_ZOOM_IN) {
    con_set_scale(con.scale + 1);
    return;
  }
  if (c == TTY_CTRL_ZOOM_OUT) {
    con_set_scale(con.scale - 1);
    return;
  }

  if (con.cx >= con.cols)
    con_newline();

  con.cells[con.cy * con.cols + con.cx] = c;
  con_draw_glyph(con.cx, con.cy, c);

  int cell_x = con.win.x + BORDER_PX + con.cx * con_cell_w();
  int cell_y = con.win.y + TITLEBAR_PX + con.cy * con_cell_h();
  mark_dirty(cell_x, cell_y, con_cell_w(), con_cell_h());

  con.cx++;
}

static void con_alloc(void) {
  int margin = 16;
  int cw = fb_w - 2 * margin - 2 * BORDER_PX;
  int ch = fb_h - 2 * margin - TITLEBAR_PX - BORDER_PX - TASKBAR_PX;
  if (cw < 64 || ch < 64)
    return;
  con_try_load_ttf();
  con.scale = CON_SCALE_MIN;
  int cell_w = con_cell_w();
  int cell_h = con_cell_h();
  cw = (cw / cell_w) * cell_w;
  ch = (ch / cell_h) * cell_h;

  size_t pixel_bytes = (size_t)cw * (size_t)ch * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;
  con.win.surface = (uint32_t *)aligned_page_alloc(pages, &con.win.surface_raw);
  if (!con.win.surface)
    return;

  fill_dwords(con.win.surface, (size_t)cw * (size_t)ch, CONSOLE_BG);

  /* Alt-screen backing buffer — same dimensions as the live surface so
   * memcpy push/pop is unconditional. Allocated once; never freed. */
  con_backing = (uint32_t *)aligned_page_alloc(pages, &con_backing_raw);
  if (!con_backing) {
    free(con.win.surface_raw);
    con.win.surface_raw = 0;
    con.win.surface = 0;
    return;
  }

  build_con_glyph_lut();
  con.win.client_w = cw;
  con.win.client_h = ch;
  con.cols = cw / cell_w;
  con.rows = ch / cell_h;
  con.cells = (char *)malloc((size_t)con.cols * (size_t)con.rows);
  con_saved_cells = (char *)malloc((size_t)con.cols * (size_t)con.rows);
  if (!con.cells || !con_saved_cells) {
    if (con.cells)
      free(con.cells);
    if (con_saved_cells)
      free(con_saved_cells);
    free(con_backing_raw);
    free(con.win.surface_raw);
    con.cells = 0;
    con_saved_cells = 0;
    con_backing = 0;
    con_backing_raw = 0;
    con.win.surface = 0;
    con.win.surface_raw = 0;
    return;
  }
  memset(con.cells, 0, (size_t)con.cols * (size_t)con.rows);
  memset(con_saved_cells, 0, (size_t)con.cols * (size_t)con.rows);
  con.win.x = margin;
  con.win.y = margin;
  con.cx = con.cy = 0;
  const char *t = "Console";
  size_t tn = 0;
  while (t[tn] && tn < sizeof(con.win.title) - 1) {
    con.win.title[tn] = t[tn];
    tn++;
  }
  con.win.title[tn] = 0;
  con.win.in_use = 1;
  z_bring_to_front(HANDLE_CONSOLE);
}

static void drain_tty_into_console(void) {
  char buf[256];
  long n;
  while ((n = tty_drain(buf, sizeof(buf))) > 0) {
    for (long i = 0; i < n; i++)
      con_putc(buf[i]);
    if (n < (long)sizeof(buf))
      break;
  }
}

/* Blit and scale an icon to dst_w x dst_h, supporting alpha and magenta chroma
 * key. */
static void blit_icon(int dst_x, int dst_y, int dst_w, int dst_h, int src_w,
                      int src_h, uint32_t *pixels) {
  if (!pixels || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
    return;

  int screen_x0 = dst_x > clip_x ? dst_x : clip_x;
  int screen_y0 = dst_y > clip_y ? dst_y : clip_y;
  if (screen_x0 < 0)
    screen_x0 = 0;
  if (screen_y0 < 0)
    screen_y0 = 0;
  int screen_x1 = dst_x + dst_w;
  int screen_y1 = dst_y + dst_h;
  if (screen_x1 > fb_w)
    screen_x1 = fb_w;
  if (screen_y1 > fb_h)
    screen_y1 = fb_h;
  if (screen_x1 > clip_x + clip_w)
    screen_x1 = clip_x + clip_w;
  if (screen_y1 > clip_y + clip_h)
    screen_y1 = clip_y + clip_h;
  if (screen_x0 >= screen_x1 || screen_y0 >= screen_y1)
    return;

  int start_y = screen_y0 - dst_y;
  int end_y = screen_y1 - dst_y;
  int start_x = screen_x0 - dst_x;
  int end_x = screen_x1 - dst_x;

  for (int y = start_y; y < end_y; y++) {
    for (int x = start_x; x < end_x; x++) {
      int sx = (x * src_w) / dst_w;
      int sy = (y * src_h) / dst_h;

      uint32_t src = pixels[sy * src_w + sx];
      uint32_t alpha = src >> 24;
      uint32_t color = src & 0x00FFFFFF;

      /* Chroma key precedes alpha because 24-bit BMPs decode opaque. */
      if (color == 0x00FF00FF) {
        continue;
      }

      if (alpha == 255) {
        *fb_pix(dst_x + x, dst_y + y) = color;
      } else if (alpha > 0) {
        blend_px(fb_pix(dst_x + x, dst_y + y), src);
      }
    }
  }
}

/* Render one entry in the stack (real window or console). Factored out so
 * compose() can iterate z_order without caring which kind it's drawing.
 * Minimized windows are skipped entirely — they keep their slot + taskbar
 * entry but contribute no pixels until restored. */
static void compose_handle(int handle) {
  if (is_minimized(handle))
    return;
  if (handle == HANDLE_CONSOLE) {
    if (!con.win.in_use)
      return;
    if (!clip_hits(con.win.x, con.win.y, outer_w_dims(con.win.client_w),
                   outer_h_dims(con.win.client_h, con.win.status_h)))
      return;
    struct window cw = {0};
    cw.x = con.win.x;
    cw.y = con.win.y;
    cw.client_w = con.win.client_w;
    cw.client_h = con.win.client_h;
    cw.surface = con.win.surface;
    memcpy(cw.title, con.win.title, sizeof(cw.title) - 1);
    draw_chrome(&cw, focused_handle == HANDLE_CONSOLE);
    blit_surface(&cw);
    return;
  }
  struct window *w = find_handle(handle);
  if (!w) {
    printf("winman: compose_handle: no window found for handle=%d\n", handle);
    return;
  }
  if (!clip_hits(w->x, w->y, outer_w(w), outer_h(w)))
    return;
  draw_chrome(w, w->handle == focused_handle);
  blit_surface(w);
}

static void compose(void) {
  if (clip_w <= 0 || clip_h <= 0)
    return;

  int cx0 = clip_x, cy0 = clip_y;
  int cx1 = clip_x + clip_w, cy1 = clip_y + clip_h;

  if (wallpaper_loaded) {
    int w = wallpaper_img.width;
    int h = wallpaper_img.height;

    for (int y = cy0; y < cy1; y++) {
      uint32_t *src_row = &wallpaper_img.pixels[(y % h) * w];
      uint32_t *dst_row = fb_pix(0, y);

      int x = cx0;
      while (x < cx1) {
        int col = x % w;
        int chunk = w - col;
        if (x + chunk > cx1)
          chunk = cx1 - x;
        memcpy(dst_row + x, src_row + col, (size_t)chunk * 4);
        x += chunk;
      }
    }
  } else {
    fb_fill_rect(cx0, cy0, cx1 - cx0, cy1 - cy0, DESKTOP_BG);
  }

  for (int i = 0; i < desktop_icon_count; i++) {
    /* Label sits under the icon, so extend the cull box downward. */
    if (!clip_hits(desktop_icons[i].x, desktop_icons[i].y, desktop_icons[i].w,
                   desktop_icons[i].h + 2 + FONT_GLYPH_H))
      continue;
    if (desktop_icons[i].loaded) {
      blit_icon(desktop_icons[i].x, desktop_icons[i].y, desktop_icons[i].w,
                desktop_icons[i].h, desktop_icons[i].icon.width,
                desktop_icons[i].icon.height, desktop_icons[i].icon.pixels);
    } else {
      fb_fill_rect(desktop_icons[i].x, desktop_icons[i].y, desktop_icons[i].w,
                   desktop_icons[i].h, 0x00808080);
    }

    int text_y = desktop_icons[i].y + desktop_icons[i].h + 2;
    draw_text_fb(desktop_icons[i].x, text_y, desktop_icons[i].program.name,
                 desktop_icons[i].w, 0x00FFFFFF, DESKTOP_BG);
  }
  for (int i = z_count - 1; i >= 0; i--) {
    compose_handle(z_order[i]);
  }

  if (clip_hits(0, taskbar_y(), fb_w, TASKBAR_PX))
    draw_taskbar();
  draw_start_menu();
  /* Last, so the dialog sits above the taskbar and the start menu too —
   * anything it did not cover would be clickable behind a modal. */
  draw_prompt();
}

/* Hand normal backbuffer copies to the kernel so large regions can use its AP
 * work queue. Keep the direct path as a fallback for an older kernel or a
 * rejected request. */
static void present_backbuffer_rects(const struct fb_rect *rects,
                                     uint32_t rect_count) {
  if (rect_count == 0)
    return;
  if (fb_present(fb, (uint32_t)fb_stride * 4U, rects, rect_count) == 0)
    return;

  for (uint32_t i = 0; i < rect_count; i++) {
    const struct fb_rect *rect = &rects[i];
    for (uint32_t row = 0; row < rect->h; row++) {
      uint32_t *src = fb_pix((int)rect->x, (int)(rect->y + row));
      uint32_t *dst =
          fb_hw + (size_t)(rect->y + row) * (size_t)fb_stride + (size_t)rect->x;
      memcpy(dst, src, (size_t)rect->w * 4);
    }
    fb_damage(rect->x, rect->y, rect->w, rect->h);
  }
}

static void present_backbuffer_rect(int x, int y, int w, int h) {
  const struct fb_rect rect = {
      .x = (uint32_t)x,
      .y = (uint32_t)y,
      .w = (uint32_t)w,
      .h = (uint32_t)h,
  };
  present_backbuffer_rects(&rect, 1);
}

static void present_full_desktop(void) {
  if (!fb_hw || !fb || fb_bytes == 0)
    return;
  clip_set(0, 0, fb_w, fb_h);
  compose();
  present_backbuffer_rect(0, 0, fb_w, fb_h);
  desktop_dirty = 0;
}

static void present_dirty(void) {
  if (!fb_hw || !fb || fb_bytes == 0 || !desktop_dirty)
    return;

  // Composite only the region we are about to copy out. Everything else
  // would be discarded, and at 1280x800 redrawing the whole desktop for a
  // small dirty box was costing ~6x winman's loop rate.
  clip_set(dirty_x, dirty_y, dirty_w, dirty_h);
  compose();

  present_backbuffer_rect(dirty_x, dirty_y, dirty_w, dirty_h);

  desktop_dirty = 0;
  dirty_w = 0;
  dirty_h = 0;
}

static int cursor_scale(void) {
  int s = (fb_w > 0 ? fb_w : 720) / 1080;
  if (s < 1)
    s = 1;
  if (s > 4)
    s = 4;
  return s;
}

/* Composite one source pixel over a destination. Fully opaque and fully
 * transparent are the overwhelmingly common cases and skip the arithmetic
 * entirely; anything between gets a per-channel blend. */
static inline void blend_px(uint32_t *dst, uint32_t src) {
  uint32_t a = src >> 24;
  if (a == 0)
    return;
  if (a == 255) {
    *dst = src & 0x00FFFFFFu;
    return;
  }

  uint32_t d = *dst;
  uint32_t inv = 255u - a;
  uint32_t r = (((src >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * inv) / 255u;
  uint32_t g = (((src >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * inv) / 255u;
  uint32_t b = ((src & 0xFF) * a + (d & 0xFF) * inv) / 255u;
  *dst = (r << 16) | (g << 8) | b;
}

static int cursor_rect(int32_t x, int32_t y, int scale, struct fb_rect *rect) {
  int draw_w = cursor_w() * scale;
  int draw_h = cursor_h() * scale;
  int x0 = x < 0 ? 0 : (int)x;
  int y0 = y < 0 ? 0 : (int)y;
  int x1 = ((int)x + draw_w) > fb_w ? fb_w : (int)x + draw_w;
  int y1 = ((int)y + draw_h) > fb_h ? fb_h : (int)y + draw_h;

  if (x0 >= x1 || y0 >= y1)
    return 0;

  *rect = (struct fb_rect){
      .x = (uint32_t)x0,
      .y = (uint32_t)y0,
      .w = (uint32_t)(x1 - x0),
      .h = (uint32_t)(y1 - y0),
  };
  return 1;
}

static void draw_cursor_with_repairs(int32_t x, int32_t y,
                                     const struct fb_rect *repairs,
                                     uint32_t repair_count) {
  int scale = cursor_scale();
  int src_w = cursor_w();
  int src_h = cursor_h();
  if (src_w <= 0 || src_h <= 0 || src_w > CURSOR_MAX_SOURCE_DIM ||
      src_h > CURSOR_MAX_SOURCE_DIM)
    return;

  struct fb_rect new_rect;
  int have_new = cursor_rect(x, y, scale, &new_rect);
  if (!have_new)
    return;

  int clipped_w = (int)new_rect.w;
  int clipped_h = (int)new_rect.h;
  int x0 = (int)new_rect.x;
  int y0 = (int)new_rect.y;

  for (int yy = 0; yy < clipped_h; yy++) {
    uint32_t *p = fb_pix(x0, y0 + yy);
    memcpy(cursor_under + (size_t)yy * CURSOR_MAX_DRAW_DIM, p,
           (size_t)clipped_w * 4);

    int src_y = (y0 + yy - (int)y) / scale;

    if (cursor_img.pixels) {
      for (int xx = 0; xx < clipped_w; xx++) {
        int src_x = (x0 + xx - (int)x) / scale;
        uint32_t src = cursor_img.pixels[(size_t)src_y * src_w + src_x];

        /* Treat pure magenta as transparent for 24-bit cursor images
         * since bmp.c correctly forces alpha to 255 for opaque images. */
        if ((src & 0x00FFFFFF) == 0x00FF00FF) {
          continue;
        }

        blend_px(&p[xx], src);
      }
    } else {
      for (int xx = 0; xx < clipped_w; xx++) {
        int src_x = (x0 + xx - (int)x) / scale;
        uint8_t mv = fallback_cursor_mask[src_y][src_x];
        if (mv == 1)
          p[xx] = COLOR_BORDER;
        else if (mv == 2)
          p[xx] = COLOR_FILL;
      }
    }
  }

  struct fb_rect rects[FB_PRESENT_MAX_RECTS];
  uint32_t rect_count = 0;
  for (uint32_t i = 0;
       i < repair_count && rect_count + 1 < FB_PRESENT_MAX_RECTS; i++) {
    if (repairs[i].w && repairs[i].h)
      rects[rect_count++] = repairs[i];
  }
  rects[rect_count++] = new_rect;

  present_backbuffer_rects(rects, rect_count);

  for (uint32_t yy = 0; yy < new_rect.h; yy++) {
    memcpy(fb_pix((int)new_rect.x, (int)(new_rect.y + yy)),
           cursor_under + (size_t)yy * CURSOR_MAX_DRAW_DIM,
           (size_t)new_rect.w * 4);
  }
}

static void draw_cursor(int32_t x, int32_t y) {
  draw_cursor_with_repairs(x, y, 0, 0);
}

static void present_cursor_repair_at(int32_t x, int32_t y) {
  struct fb_rect rect;
  if (cursor_rect(x, y, cursor_scale(), &rect))
    present_backbuffer_rects(&rect, 1);
}

static void present_rect(int x, int y, int w, int h) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fb_w)
    w = fb_w - x;
  if (y + h > fb_h)
    h = fb_h - y;
  if (w <= 0 || h <= 0)
    return;
  present_backbuffer_rect(x, y, w, h);
}

/* Restore a ghost outline from the frozen backbuffer. */
static void erase_ghost(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  present_rect(x, y, w, 1);         /* top    */
  present_rect(x, y + h - 1, w, 1); /* bottom */
  present_rect(x, y, 1, h);         /* left   */
  present_rect(x + w - 1, y, 1, h); /* right  */
}

/* Draw directly to hardware without changing the backbuffer. */
static void draw_ghost(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  uint32_t c = 0x00FFFFFFu;
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > fb_w)
    x1 = fb_w;
  if (y1 > fb_h)
    y1 = fb_h;
  if (x0 >= x1 || y0 >= y1)
    return;

  if (y >= 0 && y < fb_h) {
    uint32_t *row = fb_hw + (size_t)y * (size_t)fb_stride;
    for (int xx = x0; xx < x1; xx++)
      row[xx] = c;
  }
  int yb = y + h - 1;
  if (yb >= 0 && yb < fb_h) {
    uint32_t *row = fb_hw + (size_t)yb * (size_t)fb_stride;
    for (int xx = x0; xx < x1; xx++)
      row[xx] = c;
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
  /* Damage only the strips actually written. The kernel keeps a small set
   * of rects now, so a window-sized outline costs its perimeter instead of
   * its area. */
  if (y >= 0 && y < fb_h)
    fb_damage((uint32_t)x0, (uint32_t)y, (uint32_t)(x1 - x0), 1);
  if (yb >= 0 && yb < fb_h)
    fb_damage((uint32_t)x0, (uint32_t)yb, (uint32_t)(x1 - x0), 1);
  if (x >= 0 && x < fb_w)
    fb_damage((uint32_t)x, (uint32_t)y0, 1, (uint32_t)(y1 - y0));
  if (xr >= 0 && xr < fb_w)
    fb_damage((uint32_t)xr, (uint32_t)y0, 1, (uint32_t)(y1 - y0));
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
    *gx = nx;
    *gy = ny;
    *gw = outer_w_dims(drag.orig_cw);
    *gh = outer_h_dims(drag.orig_ch, status_h_of(drag.handle));
  } else {
    int ncw = drag.orig_cw + dx;
    int nch = drag.orig_ch + dy;
    clamp_client_size(&ncw, &nch);
    *gx = drag.orig_x;
    *gy = drag.orig_y;
    *gw = outer_w_dims(ncw);
    *gh = outer_h_dims(nch, status_h_of(drag.handle));
  }
}

static struct window *find_slot(void) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (!windows[i].in_use)
      return &windows[i];
  }
  return 0;
}

static struct window *find_handle(int handle) {
  if (handle == HANDLE_CONSOLE)
    return con.win.in_use ? &con.win : NULL;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle)
      return &windows[i];
  }
  return NULL;
}

static int handle_create(int client_pid, int w, int h, const char *title,
                         uint32_t flags, uint64_t *out_client_va,
                         uint32_t *out_pitch, int *out_handle) {
  if (w <= 0 || h <= 0 || w > CLIENT_DIM_HARD_LIMIT ||
      h > CLIENT_DIM_HARD_LIMIT)
    return -1;
  struct window *win = find_slot();
  if (!win) {
    printf("winman: handle_create: no slot found\n");
    return -1;
  }

  size_t pixel_bytes = (size_t)w * (size_t)h * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *raw = 0;
  uint32_t *surface = (uint32_t *)aligned_page_alloc(pages, &raw);
  if (!surface)
    return -1;

  /* Map the same physical pages into the client's PML4 so the client
   * can write pixels without going through winman. */
  uint64_t client_va = 0;
  if (shmem_share(client_pid, (uint64_t)surface, (long)pages, &client_va) !=
      0) {
    free(raw);
    return -1;
  }

  win->in_use = 1;
  /* Handles remain stable until their slot is reused. */
  win->handle = (int)(win - windows) + 1;
  win->owner_pid = client_pid;
  /* Cascade new windows. */
  int placed = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (&windows[i] != win && windows[i].in_use)
      placed++;
  }
  win->x = 60 + placed * 24;
  win->y = 60 + placed * 24;
  win->client_w = w;
  win->client_h = h;
  win->surface = surface;
  win->surface_raw = raw;
  win->n_pages = (int)pages;
  win->client_va = client_va;
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

  /* Opt-in status strip. Set before the first mark_dirty below so the
   * initial damage rect covers the taller frame. */
  win->status_h = (flags & WM_CREATE_STATUSBAR) ? STATUSBAR_PX : 0;
  win->status[0] = 0;

  focused_handle = win->handle;
  z_bring_to_front(win->handle);
  mark_dirty(win->x, win->y, outer_w(win), outer_h(win));

  *out_client_va = client_va;
  *out_pitch = (uint32_t)(w * 4);
  *out_handle = win->handle;
  printf("winman: create handle=%d owner=%d %dx%d pages=%d client_va=%lx "
         "active=%d\n",
         win->handle, client_pid, w, h, (int)pages, (unsigned long)client_va,
         window_count());
  return 0;
}

/* Count of slots currently in_use. Used by the taskbar enumeration and
 * for debug logging on create/destroy/reap. */
static int window_count(void) {
  int n = 0;
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (windows[i].in_use)
      n++;
  return n;
}

/* Tear down a window. Owner-check by client_pid is skipped when
 * client_pid == 0 — used by the reaper for dead-client cleanup, since the
 * dead owner can no longer issue the destroy itself. */
static void handle_destroy_internal(int handle, int client_pid_check) {
  struct window *w = find_handle(handle);
  if (!w) {
    printf("winman: handle_destroy_internal: no window found for handle=%d\n",
           handle);
    return;
  }
  if (client_pid_check && w->owner_pid != client_pid_check)
    return;

  /* Preserve cleanup state before clearing the window. */
  int old_x = w->x;
  int old_y = w->y;
  int old_ow = outer_w(w);
  int old_oh = outer_h(w);
  int old_owner_pid = w->owner_pid;
  uint64_t old_client_va = w->client_va;
  int old_n_pages = w->n_pages;
  void *old_surface_raw = w->surface_raw;

  /* Revoke the receiver mapping before making the owner allocation reusable.
   * A client may close one window and remain alive, so relying on process exit
   * to discard its PML4 leaves a stale alias into the next window allocated in
   * this heap block. If the owner is already gone, unshare fails harmlessly:
   * its address space has either been reaped or is about to be reaped. */
  if (old_client_va && old_n_pages > 0)
    shmem_unshare(old_owner_pid, old_client_va, old_n_pages);
  if (old_surface_raw)
    free(old_surface_raw);
  memset(w, 0, sizeof(*w));
  z_remove(handle);

  /* Drop any outstanding close request: this slot's handle is free for
   * reuse, and a stale pending entry would force-destroy whichever window
   * inherits it on the user's next single close click. */
  if (close_pending_handle == handle)
    close_pending_handle = -1;

  /* A dialog owned by this window has nothing left to answer to. */
  if (old_owner_pid > 0)
    prompt_abandon_for_owner(old_owner_pid);

  if (focused_handle == handle) {
    focused_handle = z_count > 0 ? z_order[0] : 0;

    /* Force taskbar redraw because the active button changed */
    mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

    /* Force new focused window to redraw its titlebar color */
    if (focused_handle) {
      struct window *fw = find_handle(focused_handle);
      if (fw)
        mark_dirty(fw->x, fw->y, outer_w(fw), outer_h(fw));
    }
  }

  mark_dirty(old_x, old_y, old_ow, old_oh);
  printf("winman: destroy handle=%d owner=%d active=%d\n", handle,
         old_owner_pid, window_count());
}

static void handle_destroy(int client_pid, int handle) {
  handle_destroy_internal(handle, client_pid);
}

static void destroy_windows_for_owner(int owner_pid, const char *why) {
  if (owner_pid <= 0)
    return;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (!windows[i].in_use || windows[i].owner_pid != owner_pid)
      continue;
    int h = windows[i].handle;
    printf("winman: reap window %d owner_pid=%d (%s)\n", h, owner_pid,
           why ? why : "owner-exit");
    handle_destroy_internal(h, 0);
  }
}

/* True if window is currently minimized (and therefore must skip compose +
 * hit-test). Console can't be minimized. */
static int is_minimized(int handle) {
  // if (handle == HANDLE_CONSOLE)
  //   return 0;
  struct window *w = find_handle(handle);
  return w && w->minimized;
}

/* Toggle min/restore. While minimized the window stays on the taskbar and
 * keeps its z-order slot, but compose + hit-test skip it; the taskbar
 * button is the only way to bring it back. */
static void toggle_minimize(int handle) {
  // if (handle == HANDLE_CONSOLE)
  //   return;
  struct window *w = find_handle(handle);
  if (!w) {
    printf("winman: toggle_minimize: no window found for handle=%d\n", handle);
    return;
  }
  w->minimized = !w->minimized;
  if (w->minimized && focused_handle == handle) {
    /* Refocus to next visible handle in z-order, preferring real
     * windows; falls back to console if nothing else qualifies. */
    focused_handle = 0;
    for (int i = 0; i < z_count; i++) {
      int h = z_order[i];
      if (h == handle)
        continue;
      if (is_minimized(h))
        continue;
      focused_handle = h;
      break;
    }
  }
  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));
}

/* Toggle maximize/restore. Saves pre-max geometry in window struct so the
 * second click restores. Uses client_window_resize so the client gets a
 * WM_RESIZE_NOTIFY with the new shared surface. */
static void client_window_resize(int handle, int new_cw, int new_ch);
static void win_set_pos(int handle, int x, int y);

static void toggle_maximize(int handle) {
  // if (handle == HANDLE_CONSOLE)
  //   return;
  struct window *w = find_handle(handle);
  if (!w) {
    printf("winman: toggle_maximize: no window found for handle=%d\n", handle);
    return;
  }
  if (w->maximized) {
    client_window_resize(handle, w->saved_cw, w->saved_ch);
    win_set_pos(handle, w->saved_x, w->saved_y);
    w->maximized = 0;
  } else {
    w->saved_x = w->x;
    w->saved_y = w->y;
    w->saved_cw = w->client_w;
    w->saved_ch = w->client_h;
    win_set_pos(handle, 0, 0);
    client_window_resize(handle, max_client_w, max_client_h);
    w->maximized = 1;
  }
  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));
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
  /* Increased to 256 so we don't miss processes if the system is busy */
  struct proc_info procs[256];
  long n = proc_list(procs, (long)(sizeof(procs) / sizeof(procs[0])));
  if (n < 0)
    return;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (!windows[i].in_use)
      continue;
    int owner = windows[i].owner_pid;
    int terminal = 1; /* Assume dead if not found in the list */
    for (long j = 0; j < n; j++) {
      if (procs[j].pid == owner) {
        int s = procs[j].state;
        terminal = (s == PROC_STATE_ZOMBIE || s == PROC_STATE_DEAD);
        break;
      }
    }
    if (terminal) {
      int h = windows[i].handle;
      printf("winman: reap window %d owner_pid=%d (dead)\n", h, owner);
      handle_destroy_internal(h, 0);
    }
  }
}

static void handle_set_title(int owner_pid, int handle, const char *title) {
  struct window *w = find_handle(handle);
  if (!w || w->owner_pid != owner_pid || !title)
    return;
  size_t i = 0;
  while (i < sizeof(w->title) - 1 && title[i]) {
    w->title[i] = title[i];
    i++;
  }
  w->title[i] = 0;
  mark_dirty(w->x, w->y, outer_w(w), outer_h(w));
}

/* ---------------- Modal prompt ------------------------------------------
 *
 * Winman owns the dialog outright: it draws it, it consumes every keystroke
 * and click while it is up, and it sends the answer back. The requesting app
 * is parked inside wm_prompt() and never sees the input, which is what makes
 * the dialog genuinely modal rather than merely painted on top.
 *
 * Only one is allowed at a time. A second request while one is open is
 * refused with WM_PROMPT_CANCEL rather than queued — an app blocked in
 * wm_prompt() cannot have issued it, so it came from a second app, and
 * stacking dialogs from unrelated apps has no sane z-order answer. */
static struct prompt_state prompt;

/* Shift state for the dialog's text field. Tracked here rather than reusing
 * the client-facing modifier state, because keystrokes that reach a prompt
 * never reach a client and vice versa. */
static int shift_held = 0;
/* Ctrl state, for the console zoom shortcuts. */
static int ctrl_held = 0;

static const char *prompt_button_label(int kind, int idx) {
  if (kind == WM_PROMPT_CONFIRM) {
    static const char *labels[] = {"Yes", "No", "Cancel"};
    return (idx >= 0 && idx < 3) ? labels[idx] : "";
  }
  if (kind == WM_PROMPT_TEXT) {
    static const char *labels[] = {"OK", "Cancel"};
    return (idx >= 0 && idx < 2) ? labels[idx] : "";
  }
  return idx == 0 ? "OK" : "";
}

static int prompt_button_count(int kind) {
  if (kind == WM_PROMPT_CONFIRM)
    return 3;
  if (kind == WM_PROMPT_TEXT)
    return 2;
  return 1;
}

/* Result for a given button index, so the click path and the keyboard path
 * cannot disagree about what "the second button" means. */
static int prompt_button_result(int kind, int idx) {
  if (kind == WM_PROMPT_CONFIRM)
    return idx == 0 ? WM_PROMPT_OK
                    : (idx == 1 ? WM_PROMPT_NO : WM_PROMPT_CANCEL);
  if (kind == WM_PROMPT_TEXT)
    return idx == 0 ? WM_PROMPT_OK : WM_PROMPT_CANCEL;
  return WM_PROMPT_OK;
}

/* Freeze dialog geometry so draw and damage paths use the same rect. */
static void prompt_freeze_rect(void) {
  prompt.w = PROMPT_W;
  prompt.h = PROMPT_H;

  int wx = 0, wy = 0, wcw = 0, wch = 0;

  /* Prefer the owner window; otherwise use the screen center. */
  if (prompt.owner_handle > 0 &&
      win_get_rect(prompt.owner_handle, &wx, &wy, &wcw, &wch)) {
    int o_w = outer_w_dims(wcw);
    int o_h = outer_h_dims(wch, status_h_of(prompt.owner_handle));

    prompt.x = wx + (o_w - PROMPT_W) / 2;
    prompt.y = wy + (o_h - PROMPT_H) / 2;
  } else {
    prompt.x = (fb_w - PROMPT_W) / 2;
    prompt.y = (fb_h - TASKBAR_PX - PROMPT_H) / 2;
  }

  /* Keep the dialog on-screen. */
  if (prompt.x < 0)
    prompt.x = 0;
  if (prompt.y < 0)
    prompt.y = 0;
  if (prompt.x + prompt.w > fb_w)
    prompt.x = (fb_w > prompt.w) ? (fb_w - prompt.w) : 0;
  if (prompt.y + prompt.h > fb_h - TASKBAR_PX)
    prompt.y =
        ((fb_h - TASKBAR_PX) > prompt.h) ? (fb_h - TASKBAR_PX - prompt.h) : 0;
}

static void prompt_rect(int *px, int *py, int *pw, int *ph) {
  *px = prompt.x;
  *py = prompt.y;
  *pw = prompt.w;
  *ph = prompt.h;
}

static void prompt_btn_rect(int idx, int *bx, int *by, int *bw, int *bh) {
  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  int n = prompt_button_count(prompt.kind);
  int row_w = n * PROMPT_BTN_W + (n - 1) * PROMPT_BTN_GAP;

  *bw = PROMPT_BTN_W;
  *bh = PROMPT_BTN_H;
  *bx = px + (pw - row_w) / 2 + idx * (PROMPT_BTN_W + PROMPT_BTN_GAP);
  *by = py + ph - PROMPT_PAD - PROMPT_BTN_H;
}

static void prompt_send_result(int result) {
  if (!prompt.active)
    return;

  struct ipc_msg resp;
  memset(&resp, 0, sizeof(resp));
  resp.type = IPC_WM_PROMPT_RESP;
  resp.a = result;
  if (prompt.kind == WM_PROMPT_TEXT && result == WM_PROMPT_OK) {
    size_t i = 0;
    while (i < sizeof(resp.str) - 1 && prompt.input[i]) {
      resp.str[i] = prompt.input[i];
      i++;
    }
    resp.str[i] = 0;
  }
  ipc_send(prompt.owner_pid, &resp);

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  prompt.active = 0;
  /* The dialog covered whatever was beneath it, so damage its whole rect. */
  mark_dirty(px, py, pw, ph);
}

static void handle_prompt_req(int owner_pid, int handle, int kind,
                              const char *message) {
  if (kind != WM_PROMPT_MESSAGE && kind != WM_PROMPT_CONFIRM &&
      kind != WM_PROMPT_TEXT)
    return;

  if (prompt.active) {
    /* Refuse rather than queue — see the note above. */
    struct ipc_msg busy;
    memset(&busy, 0, sizeof(busy));
    busy.type = IPC_WM_PROMPT_RESP;
    busy.a = WM_PROMPT_CANCEL;
    ipc_send(owner_pid, &busy);
    return;
  }

  memset(&prompt, 0, sizeof(prompt));
  prompt.active = 1;
  prompt.kind = kind;
  prompt.owner_pid = owner_pid;
  prompt.owner_handle = handle;
  prompt.selected = 0;
  if (message) {
    size_t i = 0;
    while (i < sizeof(prompt.message) - 1 && message[i]) {
      prompt.message[i] = message[i];
      i++;
    }
    prompt.message[i] = 0;
  }

  /* Freeze the rect while the owner window still exists. */
  prompt_freeze_rect();

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  mark_dirty(px, py, pw, ph);
  printf("winman: prompt kind=%d owner=%d handle=%d at %d,%d\n", kind,
         owner_pid, handle, px, py);
}

/* An owner that dies or loses its window mid-dialog leaves nothing to answer
 * to, so drop the dialog rather than leave it stranded on screen. */
static void prompt_abandon_for_owner(int owner_pid) {
  if (!prompt.active || prompt.owner_pid != owner_pid)
    return;

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  prompt.active = 0;
  mark_dirty(px, py, pw, ph);
}

static void draw_prompt(void) {
  if (!prompt.active)
    return;

  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  if (!clip_hits(px, py, pw, ph))
    return;

  fb_fill_rect(px, py, pw, ph, PROMPT_BG);
  /* Frame: light top/left, dark bottom/right — the same raised look the
   * taskbar buttons use. */
  fb_fill_rect(px, py, pw, 1, CHROME_TEXT);
  fb_fill_rect(px, py, 1, ph, CHROME_TEXT);
  fb_fill_rect(px, py + ph - 1, pw, 1, TITLEBAR_BG);
  fb_fill_rect(px + pw - 1, py, 1, ph, TITLEBAR_BG);

  draw_text_fb(px + PROMPT_PAD, py + PROMPT_PAD, prompt.message,
               pw - 2 * PROMPT_PAD, PROMPT_TEXT, PROMPT_BG);

  if (prompt.kind == WM_PROMPT_TEXT) {
    int fx = px + PROMPT_PAD;
    int fy = py + PROMPT_PAD + FONT_GLYPH_H + PROMPT_PAD;
    int fw = pw - 2 * PROMPT_PAD;
    fb_fill_rect(fx, fy, fw, PROMPT_FIELD_H, PROMPT_FIELD_BG);
    fb_fill_rect(fx, fy, fw, 1, TITLEBAR_BG);
    fb_fill_rect(fx, fy, 1, PROMPT_FIELD_H, TITLEBAR_BG);

    int ty = fy + (PROMPT_FIELD_H - FONT_GLYPH_H) / 2;
    draw_text_fb(fx + 3, ty, prompt.input, fw - 6, PROMPT_TEXT,
                 PROMPT_FIELD_BG);
    /* Caret sits after the last glyph; the field scrolls nowhere, so a
     * string longer than the field just runs under the right edge. */
    int caret_x = fx + 3 + prompt.caret * FONT_GLYPH_W;
    if (caret_x < fx + fw - 2)
      fb_fill_rect(caret_x, ty, 1, FONT_GLYPH_H, PROMPT_TEXT);
  }

  int n = prompt_button_count(prompt.kind);
  for (int i = 0; i < n; i++) {
    int bx, by, bw, bh;
    prompt_btn_rect(i, &bx, &by, &bw, &bh);
    int sel = (i == prompt.selected);
    uint32_t bg = sel ? PROMPT_BTN_SEL_BG : PROMPT_BTN_BG;
    uint32_t fg = sel ? PROMPT_BTN_SEL_FG : PROMPT_TEXT;
    fb_fill_rect(bx, by, bw, bh, bg);
    fb_fill_rect(bx, by, bw, 1, CHROME_TEXT);
    fb_fill_rect(bx, by + bh - 1, bw, 1, TITLEBAR_BG);

    const char *label = prompt_button_label(prompt.kind, i);
    int label_w = (int)strlen(label) * FONT_GLYPH_W;
    draw_text_fb(bx + (bw - label_w) / 2, by + (bh - FONT_GLYPH_H) / 2, label,
                 bw, fg, bg);
  }
}

/* Returns 1 when the dialog consumed the event. Every key and every click
 * is consumed while a prompt is up — that is what modal means. */
static int prompt_handle_key(int keycode, int shift) {
  if (!prompt.active)
    return 0;

  int n = prompt_button_count(prompt.kind);

  if (keycode == KEY_ESC) {
    prompt_send_result(WM_PROMPT_CANCEL);
    return 1;
  }
  if (keycode == KEY_ENTER || keycode == KEY_KPENTER) {
    prompt_send_result(prompt_button_result(prompt.kind, prompt.selected));
    return 1;
  }
  if (keycode == KEY_TAB || keycode == KEY_RIGHT) {
    prompt.selected = (prompt.selected + 1) % n;
    goto redraw;
  }
  if (keycode == KEY_LEFT) {
    prompt.selected = (prompt.selected + n - 1) % n;
    goto redraw;
  }

  if (prompt.kind == WM_PROMPT_TEXT) {
    if (keycode == KEY_BACKSPACE) {
      if (prompt.caret > 0)
        prompt.input[--prompt.caret] = 0;
      goto redraw;
    }
    char c = keymap_to_ascii(keycode, shift);
    if (c >= 32 && c <= 126 && prompt.caret < PROMPT_MAX_TEXT) {
      prompt.input[prompt.caret++] = c;
      prompt.input[prompt.caret] = 0;
      goto redraw;
    }
  }
  /* Swallow anything else: a stray key must not reach the app behind. */
  return 1;

redraw: {
  int px, py, pw, ph;
  prompt_rect(&px, &py, &pw, &ph);
  mark_dirty(px, py, pw, ph);
}
  return 1;
}

static int prompt_handle_click(int mx, int my) {
  if (!prompt.active)
    return 0;

  int n = prompt_button_count(prompt.kind);
  for (int i = 0; i < n; i++) {
    int bx, by, bw, bh;
    prompt_btn_rect(i, &bx, &by, &bw, &bh);
    if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
      prompt_send_result(prompt_button_result(prompt.kind, i));
      return 1;
    }
  }
  /* Clicks elsewhere are eaten too, so the window underneath cannot be
   * focused, dragged, or closed while the dialog is waiting. */
  return 1;
}

static void handle_set_status(int owner_pid, int handle, const char *text) {
  struct window *w = find_handle(handle);
  if (!w || w->owner_pid != owner_pid || !text)
    return;
  /* Dropped for a window with no strip: there is nowhere to draw it, and
   * silently growing the frame would move the client area out from under a
   * client that never asked for one. */
  if (w->status_h <= 0)
    return;

  size_t i = 0;
  while (i < sizeof(w->status) - 1 && text[i]) {
    w->status[i] = text[i];
    i++;
  }
  w->status[i] = 0;
  /* Only the strip changed, so damage just it rather than the whole frame. */
  mark_dirty(w->x + BORDER_PX, w->y + TITLEBAR_PX + w->client_h,
             outer_w(w) - 2 * BORDER_PX, w->status_h);
}

static void send_create_resp(int target_pid, int handle, uint64_t va,
                             uint32_t pitch, int w, int h) {
  struct ipc_msg resp;
  memset(&resp, 0, sizeof(resp));
  resp.type = IPC_WM_CREATE_RESP;
  resp.a = handle;
  resp.b = w;
  resp.c = h;
  resp.va = va;
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
      int handle = 0;
      int rc =
          handle_create(from, m.a, m.b, m.str, m.flags, &va, &pitch, &handle);
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
    case IPC_WM_INVALIDATE_REQ: {
      struct window *win = find_handle(m.a);
      if (win && win->owner_pid == from)
        mark_dirty(win->x, win->y, outer_w(win), outer_h(win));
      break;
    }
    case IPC_WM_SET_TITLE_REQ:
      handle_set_title(from, m.a, m.str);
      break;
    case IPC_WM_SET_STATUS_REQ:
      handle_set_status(from, m.a, m.str);
      break;
    case IPC_WM_PROMPT_REQ:
      handle_prompt_req(from, m.a, m.b, m.str);
      break;
    case IPC_PEER_EXITED: {
      /* Accept both peer-exit message layouts. */
      int dead_pid = (int)m.from_pid;
      if (dead_pid <= 0)
        dead_pid = (int)m.a;
      prompt_abandon_for_owner(dead_pid);
      destroy_windows_for_owner(dead_pid, "peer-exited");
      break;
    }
    default:
      break;
    }
  }
}

/* Translate input event from screen coords to the focused window's
 * client-area coords before forwarding. Clients draw into a surface that
 * starts at (0,0) so they shouldn't have to know their own position on
 * the desktop — winman is the only thing that does. */
static void forward_input(int target_pid, int win_handle, const struct msg *m) {
  if (target_pid <= 0)
    return;

  struct ipc_msg out;
  memset(&out, 0, sizeof(out));
  out.type = IPC_WM_INPUT;
  out.a = m->type;

  if (m->type == MSG_MOUSE_MOVE || m->type == MSG_MOUSE_DOWN ||
      m->type == MSG_MOUSE_UP) {
    int wx = 0, wy = 0, wcw = 0, wch = 0;
    int local_x = m->x;
    int local_y = m->y;

    if (win_get_rect(win_handle, &wx, &wy, &wcw, &wch)) {
      local_x = m->x - (wx + BORDER_PX);
      local_y = m->y - (wy + TITLEBAR_PX);
    }

    out.b = m->param; /* Mouse button state/mask */
    out.c = local_x;
    out.d = local_y;
  } else {
    /* Keyboard event: pass key parameters cleanly without transforming mouse
     * coords */
    out.b = m->param; /* Key code / ASCII value */
    out.c = m->x; /* Pass auxiliary key details (e.g. key flags or scancodes) if
                     set */
    out.d = m->y;
  }

  ipc_send(target_pid, &out);
}

static void request_window_close(int handle, u32 now) {
  struct window *w = find_handle(handle);
  if (!w || w->owner_pid <= 0)
    return;

  /* Second click on a window whose close request is still outstanding: the
   * owner had its chance to exit cleanly and did not take it, so take the
   * window away. Passing 0 for the pid check makes this unconditional —
   * the owner is not the one asking. */
  if (close_pending_handle == handle &&
      now - close_pending_tick < CLOSE_ESCALATE_TICKS) {
    printf("winman: close button -> force handle=%d owner=%d\n", handle,
           w->owner_pid);
    close_pending_handle = -1;
    handle_destroy_internal(handle, 0);
    return;
  }

  printf("winman: close button -> request handle=%d\n", handle);
  close_pending_handle = handle;
  close_pending_tick = now;

  struct ipc_msg out;
  memset(&out, 0, sizeof(out));
  out.type = IPC_WM_INPUT;
  out.a = WM_EV_QUIT;
  ipc_send(w->owner_pid, &out);
}

static void pump_input(void) {
  struct msg m;

  while (msg_get(&m)) {
    int forward = 1;

    /*
     * Modal prompt. Takes precedence over everything below — including an
     * in-flight drag — so no window can be moved, focused, or closed while
     * a dialog is waiting for an answer, and no keystroke leaks to the app
     * behind it. Mouse motion still falls through so the cursor keeps
     * tracking.
     */
    if (prompt.active) {
      if (m.type == MSG_KEY_DOWN) {
        int shift = (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT);
        if (shift)
          shift_held = 1;
        prompt_handle_key(m.param, shift_held);
        continue;
      }
      if (m.type == MSG_KEY_UP) {
        if (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT)
          shift_held = 0;
        continue;
      }
      if (m.type == MSG_MOUSE_DOWN) {
        prompt_handle_click(m.x, m.y);
        continue;
      }
      if (m.type == MSG_MOUSE_UP)
        continue;
      /* MSG_MOUSE_MOVE falls through to the cursor tracking below. */
    }

    /*
     * Active move/resize operation.
     */
    if (drag.active) {
      if (m.type == MSG_MOUSE_MOVE) {
        forward = 0;
      } else if (m.type == MSG_MOUSE_UP) {
        int dx = m.x - drag.grab_mx;
        int dy = m.y - drag.grab_my;

        if (drag.kind == HIT_TITLEBAR &&
            (dx < -DOUBLE_CLICK_SLOP || dx > DOUBLE_CLICK_SLOP ||
             dy < -DOUBLE_CLICK_SLOP || dy > DOUBLE_CLICK_SLOP))
          reset_titlebar_click();

        int old_x, old_y, old_cw, old_ch;
        if (win_get_rect(drag.handle, &old_x, &old_y, &old_cw, &old_ch)) {
          mark_dirty(old_x, old_y, outer_w_dims(old_cw),
                     outer_h_dims(old_ch, status_h_of(drag.handle)));
        }

        if (drag.kind == HIT_TITLEBAR) {
          int nx = drag.orig_x + dx;
          int ny = drag.orig_y + dy;

          clamp_to_desktop(&nx, &ny, drag.orig_cw, drag.orig_ch);

          win_set_pos(drag.handle, nx, ny);
        } else if (drag.kind == HIT_GRIP) {
          int new_cw = drag.orig_cw + dx;
          int new_ch = drag.orig_ch + dy;

          client_window_resize(drag.handle, new_cw, new_ch);
        }

        drag.active = 0;

        int new_x, new_y, new_cw, new_ch;
        if (win_get_rect(drag.handle, &new_x, &new_y, &new_cw, &new_ch)) {
          mark_dirty(new_x, new_y, outer_w_dims(new_cw),
                     outer_h_dims(new_ch, status_h_of(drag.handle)));
        }

        forward = 0;
      } else if (m.type == MSG_MOUSE_DOWN) {
        forward = 0;
      }
    }

    /*
     * Begin processing a left mouse click.
     */
    else if (m.type == MSG_MOUSE_DOWN && m.param == MOUSE_BTN_LEFT) {
      int hit_handle = 0;
      int kind = hit_test_at(m.x, m.y, &hit_handle);
      int titlebar_double_click = 0;
      if (start_menu_open && kind != HIT_START_MENU && kind != HIT_START_BTN) {
        start_menu_open = 0;
        start_menu_hover = -1;
        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
        mark_dirty(0, taskbar_y() - menu_h, START_MENU_W, menu_h);
        continue;
      }

      if (kind == HIT_TITLEBAR) {
        titlebar_double_click =
            titlebar_click_is_double(hit_handle, m.x, m.y, (u32)m.when);
      } else {
        reset_titlebar_click();
      }

      if (kind == HIT_BTN_CLOSE) {
        request_window_close(hit_handle, (u32)m.when);
        continue;
      }

      if (kind == HIT_BTN_MAX) {
        toggle_maximize(hit_handle);
        continue;
      }

      if (kind == HIT_BTN_MIN) {
        toggle_minimize(hit_handle);

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
        continue;
      }

      if (kind == HIT_START_BTN) {
        /* Damage the rect the menu currently occupies before rebuilding: if
         * the rescan changes the entry count the old, taller rect still has
         * to be repainted or its bottom rows are left on screen. */
        int old_menu_h =
            start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
        mark_dirty(0, taskbar_y() - old_menu_h, START_MENU_W,
                   old_menu_h + TASKBAR_PX);

        start_menu_open = !start_menu_open;
        start_menu_hover = -1;

        /* Rescan on open rather than per-frame: the directories can gain a
         * binary while winman runs, but a scan per repaint would hit the
         * filesystem on every hover change. */
        if (start_menu_open)
          build_start_menu_entries();

        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;

        mark_dirty(0, taskbar_y() - menu_h, START_MENU_W, menu_h + TASKBAR_PX);

        continue;
      }

      /*
       * Taskbar window button.
       */
      if (kind == HIT_TASKBAR_BTN) {
        int prev_focus = focused_handle;

        // if (hit_handle == HANDLE_CONSOLE) {
        //   /*
        //    * The console is not in windows[], so find_handle()
        //    * cannot be used for it.
        //    */
        //   focused_handle = HANDLE_CONSOLE;
        //   z_bring_to_front(HANDLE_CONSOLE);
        // } else
        {
          struct window *wt = find_handle(hit_handle);

          if (wt) {
            if (wt->minimized) {
              toggle_minimize(hit_handle);
              focused_handle = hit_handle;
              z_bring_to_front(hit_handle);
            } else if (focused_handle == hit_handle) {
              toggle_minimize(hit_handle);
              z_send_to_back(hit_handle);
            } else {
              focused_handle = hit_handle;
              z_bring_to_front(hit_handle);
            }

            mark_dirty(wt->x, wt->y, outer_w(wt), outer_h(wt));
          }
        }

        /* Repaint the old and new focus chrome. */
        int x, y, cw, ch;

        if (win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(prev_focus)));
        }

        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(hit_handle)));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        continue;
      }

      if (kind == HIT_START_MENU) {
        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;

        int menu_y = taskbar_y() - menu_h;
        int relative_y = m.y - menu_y - START_MENU_PAD;

        int clicked_item = relative_y / START_MENU_ITEM_H;

        if (clicked_item >= 0 && clicked_item < start_menu_count) {
          struct start_entry *prog = &start_menu_programs[clicked_item];

          char *argv[] = {(char *)prog->path, 0};

          long pid = spawn(prog->path, argv);

          if (pid > 0) {
            printf("winman: start menu spawned pid %ld\n", pid);
          } else {
            printf("winman: failed to spawn %s "
                   "(code %ld)\n",
                   prog->path, pid);
          }
        }

        start_menu_open = false;
        start_menu_hover = -1;

        mark_dirty(0, menu_y, START_MENU_W, menu_h);

        mark_dirty(0, taskbar_y(), TASKBAR_START_W, TASKBAR_PX);

        continue;
      }

      if (kind == HIT_TITLEBAR || kind == HIT_GRIP) {
        int x, y, cw, ch;

        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          int prev_focus = focused_handle;

          focused_handle = hit_handle;
          z_bring_to_front(hit_handle);

          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(hit_handle)));

          if (prev_focus != hit_handle) {
            int prev_x, prev_y, prev_cw, prev_ch;
            if (win_get_rect(prev_focus, &prev_x, &prev_y, &prev_cw,
                             &prev_ch)) {
              mark_dirty(prev_x, prev_y, outer_w_dims(prev_cw),
                         outer_h_dims(prev_ch, status_h_of(prev_focus)));
            }
          }

          mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

          if (titlebar_double_click) {
            printf("winman: titlebar double-click handle=%d\n", hit_handle);
            toggle_maximize(hit_handle);
            forward = 0;
            continue;
          }

          drag.active = 1;
          drag.kind = kind;
          drag.handle = hit_handle;
          drag.grab_mx = m.x;
          drag.grab_my = m.y;
          drag.orig_x = x;
          drag.orig_y = y;
          drag.orig_cw = cw;
          drag.orig_ch = ch;
        }

        forward = 0;
      }

      /*
       * Click on the status strip. Chrome, so it focuses the window like
       * any other frame click but is never forwarded to the client.
       */
      else if (kind == HIT_STATUSBAR) {
        if (focused_handle != hit_handle) {
          int prev_focus = focused_handle;
          focused_handle = hit_handle;
          z_bring_to_front(hit_handle);

          int x, y, cw, ch;
          if (win_get_rect(hit_handle, &x, &y, &cw, &ch))
            mark_dirty(x, y, outer_w_dims(cw),
                       outer_h_dims(ch, status_h_of(hit_handle)));
          if (win_get_rect(prev_focus, &x, &y, &cw, &ch))
            mark_dirty(x, y, outer_w_dims(cw),
                       outer_h_dims(ch, status_h_of(prev_focus)));
          mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
        }
        forward = 0;
      }

      /*
       * Click inside a window's client area.
       */
      else if (kind == HIT_CLIENT) {
        int prev_focus = focused_handle;

        focused_handle = hit_handle;
        z_bring_to_front(hit_handle);

        int x, y, cw, ch;

        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(hit_handle)));
        }

        if (prev_focus != hit_handle &&
            win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(prev_focus)));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        /*
         * The console consumes its input locally.
         */
        if (hit_handle == HANDLE_CONSOLE)
          forward = 0;
      }

      /*
       * Desktop background or desktop icon.
       */
      else if (kind == HIT_NONE) {
        int clicked_icon = -1;

        for (int i = 0; i < desktop_icon_count; i++) {
          if (m.x >= desktop_icons[i].x &&
              m.x < desktop_icons[i].x + desktop_icons[i].w &&
              m.y >= desktop_icons[i].y &&
              m.y < desktop_icons[i].y + desktop_icons[i].h) {
            clicked_icon = i;
            break;
          }
        }

        if (clicked_icon >= 0) {
          uint32_t current_tick = (uint32_t)m.when;

          printf("winman: clicked icon %d\n", clicked_icon);

          printf("winman: tick=%u, last_icon=%d, "
                 "last_tick=%u\n",
                 current_tick, last_icon_clicked, last_icon_click_tick);

          if (clicked_icon == last_icon_clicked &&
              current_tick - last_icon_click_tick < DOUBLE_CLICK_TICKS) {
            struct program *prog = &desktop_icons[clicked_icon].program;

            char *argv[] = {(char *)prog->path, 0};

            long pid = spawn(prog->path, argv);

            if (pid > 0) {
              printf("winman: spawned pid %ld\n", pid);
            } else {
              printf("winman: failed to spawn %s "
                     "(code %ld)\n",
                     prog->path, pid);
            }

            /*
             * Prevent a triple-click from launching twice.
             */
            last_icon_clicked = -1;
            last_icon_click_tick = 0;
          } else {
            last_icon_clicked = clicked_icon;
            last_icon_click_tick = current_tick;
          }
        } else {
          /*
           * Clicking elsewhere invalidates the pending
           * desktop-icon double-click.
           */
          last_icon_clicked = -1;
          last_icon_click_tick = 0;
        }

        /*
         * Desktop clicks remove the current window focus.
         */
        int prev_focus = focused_handle;
        focused_handle = 0;

        int x, y, cw, ch;

        if (win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw),
                     outer_h_dims(ch, status_h_of(prev_focus)));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        forward = 0;
      }
    } else if (m.type == MSG_KEY_DOWN) {
      // Track Alt key state
      if (m.param == KEY_LEFTALT || m.param == KEY_RIGHTALT) {
        alt_pressed = 1;
      }

      // Alt+F4 closes the focused window
      if (m.param == KEY_F4 && alt_pressed && focused_handle > 0 &&
          focused_handle != HANDLE_CONSOLE) {

        request_window_close(focused_handle, (u32)m.when);
        forward = 0; // Consume the event
        continue;
      }

      // Escape closes the start menu
      if (m.param == KEY_ESC && start_menu_open) {
        start_menu_open = 0;
        start_menu_hover = -1;
        int menu_h = start_menu_count * START_MENU_ITEM_H + START_MENU_PAD * 2;
        mark_dirty(0, taskbar_y() - menu_h, START_MENU_W, menu_h);
        forward = 0; // Consume the event
        continue;
      }
    }

    /*
     * Track key releases to update modifier state.
     */
    else if (m.type == MSG_KEY_UP) {
      if (m.param == KEY_LEFTALT || m.param == KEY_RIGHTALT) {
        alt_pressed = 0;
      }
    }

    /*
     * Consume events handled by the window manager.
     */
    if (!forward)
      continue;

    /*
     * Console input is handled by the window manager rather than
     * forwarded to a client process.
     */
    if (focused_handle == HANDLE_CONSOLE) {
      if (m.type == MSG_KEY_DOWN) {
        if (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT) {
          shift_held = 1;
        }
        if (m.param == KEY_LEFTCTRL || m.param == KEY_RIGHTCTRL) {
          ctrl_held = 1;
        }

        /* Console zoom lives here rather than in the shell. The shell now
         * receives ASCII from the TTY ring and never sees a raw keycode, so
         * it cannot spot Ctrl+-/Ctrl+= any more — and winman owns the
         * console's scale anyway. */
        if (ctrl_held && m.param == KEY_MINUS) {
          con_set_scale(con.scale - 1);
          continue;
        }
        if (ctrl_held && m.param == KEY_EQUAL) {
          con_set_scale(con.scale + 1);
          continue;
        }

        char c = keymap_to_ascii(m.param, shift_held);
        if (c != 0) {
          tty_inject(c); // Push to the kernel's TTY input buffer!
        }
      } else if (m.type == MSG_KEY_UP) {
        if (m.param == KEY_LEFTSHIFT || m.param == KEY_RIGHTSHIFT) {
          shift_held = 0;
        }
        if (m.param == KEY_LEFTCTRL || m.param == KEY_RIGHTCTRL) {
          ctrl_held = 0;
        }
      }
      continue;
    }

    /*
     * Forward unhandled input to the focused client window.
     */
    struct window *focus = focused_handle ? find_handle(focused_handle) : 0;

    if (focus) {
      forward_input(focus->owner_pid, focus->handle, &m);
    }
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("winman: start\n");
  if (wm_register() != 0) {
    printf("winman: wm_register failed\n");
    return 1;
  }
  printf("winman: registered\n");

  struct fb_info info;
  if (fb_info(&info) != 0)
    return 2;
  fb_hw = (uint32_t *)fb_map();
  if (!fb_hw)
    return 3;

  fb_w = (int)info.width;
  fb_h = (int)info.height;
  fb_stride = (int)(info.pitch / 4);
  fb_bytes = info.pitch * info.height;
  fb_mapped_bytes = fb_bytes;
  update_client_size_limits();
  printf("winman: fb %dx%d pitch=%d bytes=%d\n", fb_w, fb_h, fb_stride * 4,
         (int)fb_bytes);

  if (backbuffer_reserve(fb_bytes) != 0) {
    printf("winman: back buffer alloc failed\n");
    return 4;
  }
  if (backbuffer_register() != 0)
    printf("winman: back buffer registration failed, using fallback\n");
  printf("winman: back buffer @%p bytes=%d\n", (void *)fb, (int)fb_capacity);

  cursor_load();
  tb_load_start_icon();
  desktop_load();
  /* After fb_h is known — the menu's capacity depends on the screen height. */
  build_start_menu_entries();
  /* Populate before the first compose so the taskbar paints a real time
   * instead of a blank strip that fills in a second later. */
  clock_format();

  memset(windows, 0, sizeof(windows));
  focused_handle = 0;
  con_alloc();
  printf("winman: con.win.in_use=%d surface=%p w=%d h=%d\n", con.win.in_use,
         (void *)con.win.surface, con.win.client_w, con.win.client_h);
  present_full_desktop();
  printf("winman: ready\n");

  int self_pid = (int)get_pid();
  int tick = 0;
  int32_t last_cx = 0, last_cy = 0;
  int have_last = 0;

  for (;;) {
    if ((int)wm_pid() != self_pid) {
      /* Another process may replace the kernel's single display registration
       * while it owns the framebuffer. Re-establish ours on return. */
      fb_registered = 0;
      have_last = 0;
      sleep_ticks(1);
      continue;
    }
    if (!fb_registered && backbuffer_register() != 0)
      sleep_ticks(1);

    /* Host-driven resize: kernel may have re-pointed the scanout at a
     * different-sized backing under us. Re-query dims; on change, rebind
     * USER_FB_BASE (sys_fb_map walks the new scatter-gather page list)
     * and grow/shrink our back buffer to match. */
    {
      struct fb_info cur;
      if (fb_info(&cur) == 0 &&
          ((int)cur.width != fb_w || (int)cur.height != fb_h ||
           (int)(cur.pitch / 4) != fb_stride)) {
        size_t new_bytes = cur.pitch * cur.height;
        uint32_t *new_fb_hw = fb_hw;

        /* The physical pool is stable. Only growing beyond the prefix this
         * process already mapped needs another fb_map syscall. */
        if (new_bytes > fb_mapped_bytes) {
          new_fb_hw = (uint32_t *)fb_map();
          if (new_fb_hw)
            fb_mapped_bytes = new_bytes;
        }

        if (new_fb_hw && backbuffer_reserve(new_bytes) == 0) {
          fb_hw = new_fb_hw;
          fb_w = (int)cur.width;
          fb_h = (int)cur.height;
          fb_stride = (int)(cur.pitch / 4);
          fb_bytes = new_bytes;
          update_client_size_limits();
          if (backbuffer_register() != 0)
            printf("winman: resized back buffer registration failed\n");
          present_full_desktop();
          have_last = 0;
          printf("winman: rebound fb to %dx%d\n", fb_w, fb_h);
        }
      }
    }

    pump_ipc();
    pump_input();
    drain_tty_into_console();

    /* The taskbar is outside most dirty boxes, so nothing else would repaint
     * it. Track what it renders from and dirty the strip when that changes,
     * otherwise a new window's button never appears. */
    {
      static uint64_t taskbar_sig_prev;
      uint64_t sig = (uint64_t)(uint32_t)focused_handle * 1000003u;
      sig = sig * 31 + (uint64_t)con.win.in_use;
      for (int i = 0; i < MAX_WINDOWS; i++) {
        sig = sig * 31 + (uint64_t)windows[i].in_use;
        if (!windows[i].in_use)
          continue;
        sig = sig * 31 + (uint64_t)windows[i].handle;
        sig = sig * 31 + (uint64_t)windows[i].minimized;
        for (const char *t = windows[i].title; *t; t++)
          sig = sig * 31 + (unsigned char)*t;
      }
      if (sig != taskbar_sig_prev) {
        taskbar_sig_prev = sig;
        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
      }
    }

    /* Advance once per iteration, not once per repaint. This used to
     * live inside the `desktop_dirty && (++tick % 2)` test below, where
     * short-circuit evaluation froze it on an idle desktop — turning
     * the reap check into a constant that either fired every frame or
     * never fired at all, depending on where it stopped. */
    tick++;

    /* Reap windows whose owners have died without sending DESTROY_REQ.
     * Hot path runs the syscall (proc_list) ~every 64 ticks so the
     * common case stays cheap. */
    if ((tick & 63) == 0)
      reap_dead_windows();

    /* Clock. Polls the RTC on an interval rather than every frame, and only
     * damages the strip when the rendered text differs — an idle desktop
     * stays idle for the 59 seconds when nothing has changed. */
    if ((uint32_t)tick % CLOCK_POLL_TICKS == 0)
      clock_tick();

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
        erase_ghost(drag.last_gx, drag.last_gy, drag.last_gw, drag.last_gh);
      }
      int cursor_moved = have_last && (mx != last_cx || my != last_cy);
      int cursor_refresh = !have_last || cursor_moved || ((tick & 127) == 0);
      if (cursor_moved)
        present_cursor_repair_at(last_cx, last_cy);
      int gx, gy, gw, gh;
      compute_ghost((int)mx, (int)my, &gx, &gy, &gw, &gh);
      draw_ghost(gx, gy, gw, gh);
      drag.last_gx = gx;
      drag.last_gy = gy;
      drag.last_gw = gw;
      drag.last_gh = gh;
      drag.have_ghost = 1;

      if (cursor_refresh)
        draw_cursor(mx, my);
      last_cx = mx;
      last_cy = my;
      have_last = 1;
      yield();
      continue;
    }

    /* Drag just ended this tick — wipe the lingering ghost outline
     * before the normal compose path runs. desktop_dirty was set by
     * MOUSE_UP so the next branch will repaint everything anyway. */
    if (drag.have_ghost) {
      erase_ghost(drag.last_gx, drag.last_gy, drag.last_gw, drag.last_gh);
      drag.have_ghost = 0;
    }

    int recompose = desktop_dirty;
    if (recompose) {
      present_dirty();
    }

    int cursor_moved = have_last && (mx != last_cx || my != last_cy);
    struct fb_rect cursor_repairs[1];
    uint32_t cursor_repair_count = 0;
    if (cursor_moved &&
        cursor_rect(last_cx, last_cy, cursor_scale(), &cursor_repairs[0]))
      cursor_repair_count = 1;
    if (!have_last || recompose || cursor_moved)
      draw_cursor_with_repairs(mx, my, cursor_repairs, cursor_repair_count);
    last_cx = mx;
    last_cy = my;
    have_last = 1;

    yield();
  }
  return 0;
}
