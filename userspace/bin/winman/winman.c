#include "./winman.h"
#include "display/print.h"

static int cursor_w(void) {
  return cursor_img.pixels ? cursor_img.width : CURSOR_W;
}

static int cursor_h(void) {
  return cursor_img.pixels ? cursor_img.height : CURSOR_H;
}

/* Double-click tracking */
static int last_clicked_icon = -1;
static u32 last_click_tick = 0;

/* Try the on-disk cursor once at startup. Failure is not an error worth
 * stopping for — it just leaves the built-in mask in place. */
static void cursor_load(void) {
  if (bmp_load(CURSOR_BMP_PATH, &cursor_img) == 0) {
    printf("winman: cursor %dx%d from %s\n", cursor_img.width,
           cursor_img.height, CURSOR_BMP_PATH);
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

  desktop_icons[0].x = 20;
  desktop_icons[0].y = 20;
  desktop_icons[0].program.name = "DOOM";
  desktop_icons[0].program.path = "/usr/bin/doom.elf";

  desktop_icons[1].x = 20;
  desktop_icons[1].y = 100;
  desktop_icons[1].program.name = "shelf";
  desktop_icons[1].program.path = "/bin/sh.elf";

  desktop_icon_count = 2;

  /* Load Icon Images */
  for (int i = 0; i < desktop_icon_count; i++) {
    char path[64];
    // Create strings like "/system/icons/DOOM.bmp"
    // If you don't have snprintf, just hardcode the strings!
    int n = 0;
    const char *prefix = "/system/icons/";
    while (prefix[n] && n < 40) {
      path[n] = prefix[n];
      n++;
    }
    int m = 0;
    while (desktop_icons[i].program.name[m] && n < 60) {
      path[n] = desktop_icons[i].program.name[m];
      n++;
      m++;
    }
    path[n++] = '.';
    path[n++] = 'b';
    path[n++] = 'm';
    path[n++] = 'p';
    path[n++] = 0;

    if (bmp_load(path, &desktop_icons[i].icon) == 0) {
      desktop_icons[i].loaded = 1;
      desktop_icons[i].w = 32; // desktop_icons[i].icon.width;
      desktop_icons[i].h = 32; // desktop_icons[i].icon.height;
    } else {
      printf("winman: icon %s not found, using fallback\n",
             desktop_icons[i].program.name);
      desktop_icons[i].loaded = 0;
      desktop_icons[i].w = 32;
      desktop_icons[i].h = 32;
    }
  }
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
  return w->client_h + TITLEBAR_PX + BORDER_PX;
}

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
    uint64_t *p = (uint64_t *)dst;
    size_t pairs = n >> 1;
    for (size_t i = 0; i < pairs; i++)
      p[i] = v;
    dst += pairs * 2;
    n &= 1;
  }
  while (n--)
    *dst++ = color;
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
    if (!con.enabled)
      return 0;
    *x = con.x;
    *y = con.y;
    *cw = con.client_w;
    *ch = con.client_h;
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
    con.x = x;
    con.y = y;
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
  if (con.enabled && n < max) {
    out[n].handle = HANDLE_CONSOLE;
    out[n].title = con.title;
    n++;
  }
  for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
    if (!windows[i].in_use)
      continue;
    out[n].handle = windows[i].handle;
    out[n].title = windows[i].title;
    n++;
  }
  return n;
}

static int taskbar_y(void) { return fb_h - TASKBAR_PX; }

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
    if (bx + TASKBAR_BTN_W > fb_w)
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
  int menu_h = START_MENU_COUNT * START_MENU_ITEM_H + START_MENU_PAD * 2;
  return mx >= menu_x && mx < menu_x + START_MENU_W && my >= menu_y &&
         my < menu_y + menu_h;
}

/* Classify a screen-space hit by walking the z-order topmost-first. The
 * console is just another z-stack entry now; whichever handle is at
 * z_order[0] wins ties at the same pixel. */
static int hit_test_at(int mx, int my, int *out_handle) {
  /* GLOBAL UI CHECKS FIRST */
  if (start_menu_open) {
    int menu_x = 0;
    int menu_h = START_MENU_COUNT * START_MENU_ITEM_H + START_MENU_PAD * 2;
    int menu_y = taskbar_y() - menu_h;
    if (in_start_menu(menu_x, menu_y, mx, my)) {
      return HIT_START_MENU;
    }
  }

  /* TASKBAR CHECKS */
  if (my >= taskbar_y()) {
    // Check Start Button
    if (mx >= 0 && mx < TASKBAR_START_W) {
      return HIT_START_BTN;
    }
    // Check Taskbar Window Buttons
    int tb_handle = 0;
    if (hit_taskbar(mx, my, &tb_handle)) {
      *out_handle = tb_handle;
      return HIT_TASKBAR_BTN; // Make sure this is defined in winman.h!
    }
    return HIT_NONE; // Clicked on empty taskbar area
  }

  /* WINDOW CHECKS */
  for (int i = 0; i < z_count; i++) {
    int h = z_order[i];
    if (is_minimized(h))
      continue;
    int x, y, cw, ch;
    if (!win_get_rect(h, &x, &y, &cw, &ch))
      continue;
    int ow = outer_w_dims(cw);
    int oh = outer_h_dims(ch);
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
  if (!con.enabled)
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
  if (new_cw == con.client_w && new_ch == con.client_h)
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

  if (con.raw)
    free(con.raw);
  if (con_backing_raw)
    free(con_backing_raw);
  if (con.cells)
    free(con.cells);
  if (con_saved_cells)
    free(con_saved_cells);

  con.surface = new_surf;
  con.raw = new_raw;
  con.cells = new_cells;
  con_backing = new_back;
  con_backing_raw = new_back_raw;
  con_saved_cells = new_saved;
  con.client_w = new_cw;
  con.client_h = new_ch;
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
  struct window *w = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle) {
      w = &windows[i];
      break;
    }
  }
  if (!w)
    return;
  if (new_cw < MIN_CLIENT_W)
    new_cw = MIN_CLIENT_W;
  if (new_ch < MIN_CLIENT_H)
    new_ch = MIN_CLIENT_H;
  if (new_cw == w->client_w && new_ch == w->client_h)
    return;

  size_t pixel_bytes = (size_t)new_cw * (size_t)new_ch * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *raw = 0;
  uint32_t *surface = (uint32_t *)aligned_page_alloc(pages, &raw);
  if (!surface)
    return;
  memset(surface, 0, pages * 4096);

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
                                      MAP_PRIVATE | MAP_ANONYMOUS);
  if (new_fb == MAP_FAILED)
    return -1;

  if (fb && fb_capacity)
    munmap(fb, fb_capacity);
  fb = new_fb;
  fb_capacity = capacity;
  return 0;
}

static inline uint32_t *fb_pix(int x, int y) {
  return fb + (size_t)y * (size_t)fb_stride + (size_t)x;
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
  /* Single-shot rect clip instead of per-pixel bounds checks. Glyphs that
   * land fully off-screen short-circuit; glyphs that partly hang off the
   * edges trim once and then the inner loops run clean. */
  if (x + FONT_GLYPH_W <= 0 || x >= fb_w)
    return;
  if (y + FONT_GLYPH_H <= 0 || y >= fb_h)
    return;
  int col_first = x < 0 ? -x : 0;
  int col_last = x + FONT_GLYPH_W > fb_w ? fb_w - x : FONT_GLYPH_W;
  int row_first = y < 0 ? -y : 0;
  int row_last = y + FONT_GLYPH_H > fb_h ? fb_h - y : FONT_GLYPH_H;
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
      if (px < 0 || px >= fb_w || py < 0 || py >= fb_h)
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
      if (px < 0 || px >= fb_w || py < 0 || py >= fb_h)
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
}

static void blit_surface(const struct window *w) {
  int dst_x = w->x + BORDER_PX;
  int dst_y = w->y + TITLEBAR_PX;
  int cw = w->client_w;
  int ch = w->client_h;

  int x0 = dst_x < 0 ? 0 : dst_x;
  int y0 = dst_y < 0 ? 0 : dst_y;
  int x1 = (dst_x + cw) > fb_w ? fb_w : dst_x + cw;
  int y1 = (dst_y + ch) > fb_h ? fb_h : dst_y + ch;
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

static void draw_taskbar(void) {
  int y = taskbar_y();
  if (y < 0)
    return;
  // fill the entire taskbar
  fb_fill_rect(0, y, fb_w, TASKBAR_PX, TASKBAR_BG);

  // draw the start button FIRST at x=0 so it doesn't cover taskbar buttons
  // Using WHITE for fg so the > is visible against the red bg
  draw_button_mask_large(0, y, fallback_taskbar_start_mask, PRINT_COLOR_GREEN,
                         TASKBAR_BG);

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
    if (bx + TASKBAR_BTN_W > fb_w)
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
}

static void draw_start_menu(void) {
  if (!start_menu_open)
    return;

  int menu_h = START_MENU_COUNT * START_MENU_ITEM_H + START_MENU_PAD * 2;
  int mx = 0;
  int my = taskbar_y() - menu_h;

  // Background
  fb_fill_rect(mx, my, START_MENU_W, menu_h, MENU_BG);

  // Simple 1px Black Border
  fb_fill_rect(mx, my, START_MENU_W, 1, PRINT_COLOR_BLACK); // Top
  fb_fill_rect(mx, my + menu_h - 1, START_MENU_W, 1,
               PRINT_COLOR_BLACK);                    // Bottom
  fb_fill_rect(mx, my, 1, menu_h, PRINT_COLOR_BLACK); // Left
  fb_fill_rect(mx + START_MENU_W - 1, my, 1, menu_h,
               PRINT_COLOR_BLACK); // Right

  // Draw the programs
  for (int i = 0; i < START_MENU_COUNT; i++) {
    int item_y = my + START_MENU_PAD + (i * START_MENU_ITEM_H);
    int item_x = mx + START_MENU_PAD;
    int item_w = START_MENU_W - (START_MENU_PAD * 2);

    // Highlight if hovered
    uint32_t bg =
        (i == start_menu_hover) ? MENU_HOVER_BG : MENU_BG;
    uint32_t fg =
        (i == start_menu_hover) ? MENU_HOVER_FG : MENU_TEXT;

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
  if (px + cell_w > con.client_w)
    return;
  if (py + cell_h > con.client_h)
    return;

  if (con_ttf_ready) {
    for (int y = 0; y < cell_h; y++) {
      uint32_t *line =
          con.surface + (size_t)(py + y) * (size_t)con.client_w + (size_t)px;
      fill_dwords(line, (size_t)cell_w, CONSOLE_BG);
    }

    if ((unsigned char)c < 32 || (unsigned char)c > 126 || c == ' ')
      return;

    struct gfx_surface s;
    gfx_surface_init(&s, con.surface, con.client_w, con.client_h, con.client_w);
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
        con.surface + (size_t)py * (size_t)con.client_w + (size_t)px;
    for (int r = 0; r < FONT_GLYPH_H; r++) {
      memcpy(line, con_glyph_lut[glyph[r]], FONT_GLYPH_W * sizeof(uint32_t));
      line += con.client_w;
    }
    return;
  }

  for (int r = 0; r < FONT_GLYPH_H; r++) {
    for (int sy = 0; sy < con.scale; sy++) {
      int y = py + r * con.scale + sy;
      uint32_t *line =
          con.surface + (size_t)y * (size_t)con.client_w + (size_t)px;
      for (int col = 0; col < FONT_GLYPH_W; col++) {
        uint32_t color = ((glyph[r] >> col) & 1) ? CONSOLE_FG : CONSOLE_BG;
        for (int sx = 0; sx < con.scale; sx++)
          line[col * con.scale + sx] = color;
      }
    }
  }
}

static void con_redraw(void) {
  if (!con.enabled || !con.surface || !con.cells)
    return;
  fill_dwords(con.surface, (size_t)con.client_w * (size_t)con.client_h,
              CONSOLE_BG);
  for (int y = 0; y < con.rows; y++) {
    for (int x = 0; x < con.cols; x++) {
      char c = con.cells[y * con.cols + x];
      if (c)
        con_draw_glyph(x, y, c);
    }
  }
  mark_dirty(con.x, con.y, outer_w_dims(con.client_w),
             outer_h_dims(con.client_h));
}

static int con_set_scale(int new_scale) {
  if (!con.enabled)
    return -1;
  if (new_scale < CON_SCALE_MIN)
    new_scale = CON_SCALE_MIN;
  if (new_scale > CON_SCALE_MAX)
    new_scale = CON_SCALE_MAX;
  if (new_scale == con.scale)
    return con.scale;

  int new_cols = con.client_w / con_cell_w_for_scale(new_scale);
  int new_rows = con.client_h / con_cell_h_for_scale(new_scale);
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
  if (!con.enabled || !con.surface)
    return;
  fill_dwords(con.surface, (size_t)con.client_w * (size_t)con.client_h,
              CONSOLE_BG);
  if (con.cells)
    memset(con.cells, 0, (size_t)con.cols * (size_t)con.rows);
  con.cx = con.cy = 0;
  mark_dirty(con.x, con.y, outer_w_dims(con.client_w),
             outer_h_dims(con.client_h));
}

static void con_save(void) {
  if (!con.enabled || !con.surface || !con_backing)
    return;
  size_t pixels = (size_t)con.client_w * (size_t)con.client_h;
  memcpy(con_backing, con.surface, pixels * 4);
  if (con.cells && con_saved_cells)
    memcpy(con_saved_cells, con.cells, (size_t)con.cols * (size_t)con.rows);
  con_saved_cx = con.cx;
  con_saved_cy = con.cy;
  con_saved_valid = 1;
}

static void con_restore(void) {
  if (!con.enabled || !con.surface || !con_backing || !con_saved_valid)
    return;
  size_t pixels = (size_t)con.client_w * (size_t)con.client_h;
  memcpy(con.surface, con_backing, pixels * 4);
  if (con.cells && con_saved_cells)
    memcpy(con.cells, con_saved_cells, (size_t)con.cols * (size_t)con.rows);
  con.cx = con_saved_cx;
  con.cy = con_saved_cy;
  con_saved_valid = 0;
  mark_dirty(con.x, con.y, outer_w_dims(con.client_w),
             outer_h_dims(con.client_h));
}

static void con_putc(char c) {
  if (!con.enabled)
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
      int cell_x = con.x + BORDER_PX + con.cx * con_cell_w();
      int cell_y = con.y + TITLEBAR_PX + con.cy * con_cell_h();
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

  int cell_x = con.x + BORDER_PX + con.cx * con_cell_w();
  int cell_y = con.y + TITLEBAR_PX + con.cy * con_cell_h();
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
  con.surface = (uint32_t *)aligned_page_alloc(pages, &con.raw);
  if (!con.surface)
    return;

  fill_dwords(con.surface, (size_t)cw * (size_t)ch, CONSOLE_BG);

  /* Alt-screen backing buffer — same dimensions as the live surface so
   * memcpy push/pop is unconditional. Allocated once; never freed. */
  con_backing = (uint32_t *)aligned_page_alloc(pages, &con_backing_raw);
  if (!con_backing) {
    free(con.raw);
    con.raw = 0;
    con.surface = 0;
    return;
  }

  build_con_glyph_lut();
  con.client_w = cw;
  con.client_h = ch;
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
  while (t[tn] && tn < sizeof(con.title) - 1) {
    con.title[tn] = t[tn];
    tn++;
  }
  con.title[tn] = 0;
  con.enabled = 1;
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

  for (int y = 0; y < dst_h; y++) {
    for (int x = 0; x < dst_w; x++) {
      if (dst_y + y >= fb_h || dst_x + x >= fb_w)
        continue;

      /* Nearest-neighbor sampling: map destination pixel to source pixel */
      int sx = (x * src_w) / dst_w;
      int sy = (y * src_h) / dst_h;

      uint32_t src = pixels[sy * src_w + sx];
      uint32_t alpha = src >> 24;

      if (alpha == 255) {
        // Fully opaque, just copy it
        *fb_pix(dst_x + x, dst_y + y) = src & 0x00FFFFFF;
      } else if (alpha > 0) {
        // Has alpha channel, blend it
        blend_px(fb_pix(dst_x + x, dst_y + y), src);
      } else {
        // Alpha is 0. If it's pure magenta, treat it as transparent.
        if ((src & 0x00FFFFFF) == 0x00FF00FF) {
          continue; // Transparent! Skip this pixel.
        }
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
    if (!con.enabled)
      return;
    struct window cw = {0};
    cw.x = con.x;
    cw.y = con.y;
    cw.client_w = con.client_w;
    cw.client_h = con.client_h;
    cw.surface = con.surface;
    memcpy(cw.title, con.title, sizeof(cw.title) - 1);
    draw_chrome(&cw, focused_handle == HANDLE_CONSOLE);
    blit_surface(&cw);
    return;
  }
  struct window *w = find_handle(handle);
  if (!w)
    return;
  draw_chrome(w, w->handle == focused_handle);
  blit_surface(w);
}

static void compose(void) {
  /* Draw Desktop Background (Tiled) */
  if (wallpaper_loaded) {
    int w = wallpaper_img.width;
    int h = wallpaper_img.height;

    for (int y = 0; y < fb_h; y++) {
      // Get the correct source row from the wallpaper (wrapping vertically)
      uint32_t *src_row = &wallpaper_img.pixels[(y % h) * w];
      uint32_t *dst_row = fb_pix(0, y);

      int x = 0;
      // Tile horizontally across the screen
      while (x < fb_w) {
        int chunk = w;
        if (x + chunk > fb_w) {
          chunk = fb_w - x; // Don't draw past the edge of the screen
        }
        // Fast copy of a chunk of pixels
        memcpy(dst_row + x, src_row, (size_t)chunk * 4);
        x += chunk;
      }
    }
  } else {
    fb_fill_rect(0, 0, fb_w, fb_h, DESKTOP_BG); // Fallback to teal
  }

  /* Draw Desktop Icons */
  for (int i = 0; i < desktop_icon_count; i++) {
    if (desktop_icons[i].loaded) {
      // Pass target size (desktop_icons[i].w / h) and source size (img.width /
      // height)
      blit_icon(desktop_icons[i].x, desktop_icons[i].y, desktop_icons[i].w,
                desktop_icons[i].h, desktop_icons[i].icon.width,
                desktop_icons[i].icon.height, desktop_icons[i].icon.pixels);
    } else {
      // Draw a fallback grey square if the image is missing
      fb_fill_rect(desktop_icons[i].x, desktop_icons[i].y, desktop_icons[i].w,
                   desktop_icons[i].h, 0x00808080);
    }

    // Draw the text label under the icon
    int text_y = desktop_icons[i].y + desktop_icons[i].h + 2;
    draw_text_fb(desktop_icons[i].x, text_y, desktop_icons[i].program.name,
                 desktop_icons[i].w, 0x00FFFFFF, DESKTOP_BG);
  }
  /* Draw Windows back-to-front */
  for (int i = z_count - 1; i >= 0; i--) {
    compose_handle(z_order[i]);
  }

  /* Draw Taskbar (always on top) */
  draw_taskbar();
  draw_start_menu();
}

static void present_full_desktop(void) {
  if (!fb_hw || !fb || fb_bytes == 0)
    return;
  compose();
  for (int y = 0; y < fb_h; y++) {
    uint32_t *src = fb + (size_t)y * (size_t)fb_stride;
    uint32_t *dst = fb_hw + (size_t)y * (size_t)fb_stride;
    memcpy(dst, src, (size_t)fb_w * 4);
  }
  fb_damage(0, 0, (uint32_t)fb_w, (uint32_t)fb_h);
  desktop_dirty = 0;
}

static void present_dirty(void) {
  if (!fb_hw || !fb || fb_bytes == 0 || !desktop_dirty)
    return;

  // Still composite the whole screen to the back buffer.
  // (Software compositing is fast enough; the bottleneck is the memcpy).
  compose();

  // Only copy the dirty bounding box to the hardware framebuffer
  for (int yy = 0; yy < dirty_h; yy++) {
    uint32_t *src = fb_pix(dirty_x, dirty_y + yy);
    uint32_t *dst =
        fb_hw + (size_t)(dirty_y + yy) * (size_t)fb_stride + (size_t)dirty_x;
    memcpy(dst, src, (size_t)dirty_w * 4);
  }

  // Tell the kernel (and thus VirtIO) to ONLY flush this rect!
  fb_damage((uint32_t)dirty_x, (uint32_t)dirty_y, (uint32_t)dirty_w,
            (uint32_t)dirty_h);

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

static void draw_cursor(int32_t x, int32_t y) {
  int scale = cursor_scale();
  int src_w = cursor_w();
  int src_h = cursor_h();
  int draw_w = src_w * scale;
  int draw_h = src_h * scale;

  int x0 = x < 0 ? 0 : (int)x;
  int y0 = y < 0 ? 0 : (int)y;
  int x1 = ((int)x + draw_w) > fb_w ? fb_w : (int)x + draw_w;
  int y1 = ((int)y + draw_h) > fb_h ? fb_h : (int)y + draw_h;

  if (x0 >= x1 || y0 >= y1)
    return;

  for (int yy = 0; yy < y1 - y0; yy++) {
    uint32_t *p = fb_hw + (size_t)(y0 + yy) * (size_t)fb_stride + (size_t)x0;

    int src_y = (y0 + yy - (int)y) / scale;

    for (int xx = 0; xx < x1 - x0; xx++) {
      int src_x = (x0 + xx - (int)x) / scale;

      if (cursor_img.pixels) {
        blend_px(&p[xx], cursor_img.pixels[(size_t)src_y * src_w + src_x]);
      } else {
        uint8_t mv = fallback_cursor_mask[src_y][src_x];
        if (mv == 1)
          p[xx] = COLOR_BORDER;
        else if (mv == 2)
          p[xx] = COLOR_FILL;
      }
    }
  }
  fb_damage((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0),
            (uint32_t)(y1 - y0));
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
  if (w <= 0 || h <= 0)
    return;
  present_rect(x, y, w, 1);         /* top    */
  present_rect(x, y + h - 1, w, 1); /* bottom */
  present_rect(x, y, 1, h);         /* left   */
  present_rect(x + w - 1, y, 1, h); /* right  */
}

/* Draw a 1px white outline directly on fb_hw at the proposed drag position.
 * Bypasses the back buffer so the underlying composite stays untouched and
 * erase_ghost can restore from it next frame. */
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
    *gh = outer_h_dims(drag.orig_ch);
  } else {
    int ncw = drag.orig_cw + dx;
    int nch = drag.orig_ch + dy;
    if (ncw < MIN_CLIENT_W)
      ncw = MIN_CLIENT_W;
    if (nch < MIN_CLIENT_H)
      nch = MIN_CLIENT_H;
    *gx = drag.orig_x;
    *gy = drag.orig_y;
    *gw = outer_w_dims(ncw);
    *gh = outer_h_dims(nch);
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
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].in_use && windows[i].handle == handle)
      return &windows[i];
  }
  return 0;
}

static int handle_create(int client_pid, int w, int h, const char *title,
                         uint64_t *out_client_va, uint32_t *out_pitch,
                         int *out_handle) {
  if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
    return -1;
  struct window *win = find_slot();
  if (!win)
    return -1;

  size_t pixel_bytes = (size_t)w * (size_t)h * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *raw = 0;
  uint32_t *surface = (uint32_t *)aligned_page_alloc(pages, &raw);
  if (!surface)
    return -1;
  memset(surface, 0, pages * 4096);

  /* Map the same physical pages into the client's PML4 so the client
   * can write pixels without going through winman. */
  uint64_t client_va = 0;
  if (shmem_share(client_pid, (uint64_t)surface, (long)pages, &client_va) !=
      0) {
    free(raw);
    return -1;
  }

  win->in_use = 1;
  /* Handle = slot index + 1. Stable for the slot's lifetime; reused when
   * the slot is freed (find_slot reclaims it). */
  win->handle = (int)(win - windows) + 1;
  win->owner_pid = client_pid;
  /* Cascade placement: stagger new windows down-right so they don't
   * all stack at (0,0). */
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
  if (!w)
    return;
  if (client_pid_check && w->owner_pid != client_pid_check)
    return;

  /* Save geometry so we can mark the dirty rect AFTER the struct is wiped */
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
  if (handle == HANDLE_CONSOLE)
    return 0;
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
  if (!w)
    return;
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
  if (!w)
    return;
  if (w->maximized) {
    client_window_resize(handle, w->saved_cw, w->saved_ch);
    win_set_pos(handle, w->saved_x, w->saved_y);
    w->maximized = 0;
  } else {
    w->saved_x = w->x;
    w->saved_y = w->y;
    w->saved_cw = w->client_w;
    w->saved_ch = w->client_h;
    int max_cw = fb_w - 2 * BORDER_PX;
    int max_ch = fb_h - TASKBAR_PX - TITLEBAR_PX - BORDER_PX;
    win_set_pos(handle, 0, 0);
    client_window_resize(handle, max_cw, max_ch);
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
    case IPC_WM_INVALIDATE_REQ: {
      struct window *win = find_handle(m.a);
      if (win && win->owner_pid == from)
        mark_dirty(win->x, win->y, outer_w(win), outer_h(win));
      break;
    }
    case IPC_WM_SET_TITLE_REQ:
      handle_set_title(from, m.a, m.str);
      break;
    case IPC_PEER_EXITED: {
      // The dead PID might be in from_pid or m.a depending on your kernel
      int dead_pid = (int)m.from_pid;
      if (dead_pid <= 0)
        dead_pid = (int)m.a;
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
  out.a = m->type;
  out.b = m->param;
  out.c = local_x;
  out.d = local_y;
  ipc_send(target_pid, &out);
}

static void request_window_close(int handle) {
  struct window *w = find_handle(handle);
  if (!w || w->owner_pid <= 0)
    return;

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
     * Active move/resize operation.
     */
    if (drag.active) {
      if (m.type == MSG_MOUSE_MOVE) {
        forward = 0;
      } else if (m.type == MSG_MOUSE_UP) {
        int dx = m.x - drag.grab_mx;
        int dy = m.y - drag.grab_my;

        int old_x, old_y, old_cw, old_ch;
        if (win_get_rect(drag.handle, &old_x, &old_y, &old_cw, &old_ch)) {
          mark_dirty(old_x, old_y, outer_w_dims(old_cw), outer_h_dims(old_ch));
        }

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

        drag.active = 0;

        int new_x, new_y, new_cw, new_ch;
        if (win_get_rect(drag.handle, &new_x, &new_y, &new_cw, &new_ch)) {
          mark_dirty(new_x, new_y, outer_w_dims(new_cw), outer_h_dims(new_ch));
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

      /*
       * Window close button.
       */
      if (kind == HIT_BTN_CLOSE) {
        if (hit_handle != HANDLE_CONSOLE) {
          printf("winman: close button -> request handle=%d\n", hit_handle);
          request_window_close(hit_handle);
        }

        continue;
      }

      /*
       * Window maximize button.
       */
      if (kind == HIT_BTN_MAX) {
        toggle_maximize(hit_handle);
        continue;
      }

      /*
       * Window minimize button.
       */
      if (kind == HIT_BTN_MIN) {
        toggle_minimize(hit_handle);

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
        continue;
      }

      /*
       * Start button.
       */
      if (kind == HIT_START_BTN) {
        start_menu_open = !start_menu_open;
        start_menu_hover = -1;

        int menu_h = START_MENU_COUNT * START_MENU_ITEM_H + START_MENU_PAD * 2;

        mark_dirty(0, taskbar_y() - menu_h, START_MENU_W, menu_h + TASKBAR_PX);

        continue;
      }

      /*
       * Taskbar window button.
       */
      if (kind == HIT_TASKBAR_BTN) {
        int prev_focus = focused_handle;

        if (hit_handle == HANDLE_CONSOLE) {
          /*
           * The console is not in windows[], so find_handle()
           * cannot be used for it.
           */
          focused_handle = HANDLE_CONSOLE;
          z_bring_to_front(HANDLE_CONSOLE);
        } else {
          struct window *wt = find_handle(hit_handle);

          if (wt) {
            if (wt->minimized) {
              /*
               * Restore a minimized window.
               */
              toggle_minimize(hit_handle);
              focused_handle = hit_handle;
              z_bring_to_front(hit_handle);
            } else if (focused_handle == hit_handle) {
              /*
               * Clicking the focused taskbar button minimizes it.
               */
              toggle_minimize(hit_handle);
              z_send_to_back(hit_handle);
            } else {
              /*
               * Clicking an unfocused visible window raises it.
               */
              focused_handle = hit_handle;
              z_bring_to_front(hit_handle);
            }

            mark_dirty(wt->x, wt->y, outer_w(wt), outer_h(wt));
          }
        }

        /*
         * Repaint the previously focused window.
         * win_get_rect() also supports HANDLE_CONSOLE.
         */
        int x, y, cw, ch;

        if (win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw), outer_h_dims(ch));
        }

        /*
         * Repaint the newly selected taskbar window.
         */
        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw), outer_h_dims(ch));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        continue;
      }

      /*
       * Start menu item.
       */
      if (kind == HIT_START_MENU) {
        int menu_h = START_MENU_COUNT * START_MENU_ITEM_H + START_MENU_PAD * 2;

        int menu_y = taskbar_y() - menu_h;
        int relative_y = m.y - menu_y - START_MENU_PAD;

        int clicked_item = relative_y / START_MENU_ITEM_H;

        if (clicked_item >= 0 && clicked_item < START_MENU_COUNT) {
          struct program *prog = &start_menu_programs[clicked_item];

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

      /*
       * Start moving or resizing a window.
       */
      if (kind == HIT_TITLEBAR || kind == HIT_GRIP) {
        int x, y, cw, ch;

        if (win_get_rect(hit_handle, &x, &y, &cw, &ch)) {
          int prev_focus = focused_handle;

          focused_handle = hit_handle;
          z_bring_to_front(hit_handle);

          drag.active = 1;
          drag.kind = kind;
          drag.handle = hit_handle;
          drag.grab_mx = m.x;
          drag.grab_my = m.y;
          drag.orig_x = x;
          drag.orig_y = y;
          drag.orig_cw = cw;
          drag.orig_ch = ch;

          mark_dirty(x, y, outer_w_dims(cw), outer_h_dims(ch));

          if (prev_focus != hit_handle &&
              win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
            mark_dirty(x, y, outer_w_dims(cw), outer_h_dims(ch));
          }

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
          mark_dirty(x, y, outer_w_dims(cw), outer_h_dims(ch));
        }

        if (prev_focus != hit_handle &&
            win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw), outer_h_dims(ch));
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
          uint32_t current_tick = (uint32_t)get_ticks();

          printf("winman: clicked icon %d\n", clicked_icon);

          printf("winman: tick=%u, last_icon=%d, "
                 "last_tick=%u\n",
                 current_tick, last_clicked_icon, last_click_tick);

          if (clicked_icon == last_clicked_icon &&
              current_tick - last_click_tick < 370) {
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
            last_clicked_icon = -1;
            last_click_tick = 0;
          } else {
            last_clicked_icon = clicked_icon;
            last_click_tick = current_tick;
          }
        } else {
          /*
           * Clicking elsewhere invalidates the pending
           * desktop-icon double-click.
           */
          last_clicked_icon = -1;
          last_click_tick = 0;
        }

        /*
         * Desktop clicks remove the current window focus.
         */
        int prev_focus = focused_handle;
        focused_handle = 0;

        int x, y, cw, ch;

        if (win_get_rect(prev_focus, &x, &y, &cw, &ch)) {
          mark_dirty(x, y, outer_w_dims(cw), outer_h_dims(ch));
        }

        mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);

        forward = 0;
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
    if (focused_handle == HANDLE_CONSOLE)
      continue;

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
  printf("winman: fb %dx%d pitch=%d bytes=%d\n", fb_w, fb_h, fb_stride * 4,
         (int)fb_bytes);

  if (backbuffer_reserve(fb_bytes) != 0) {
    printf("winman: back buffer alloc failed\n");
    return 4;
  }
  printf("winman: back buffer @%p bytes=%d\n", (void *)fb, (int)fb_capacity);

  cursor_load();
  desktop_load();

  memset(windows, 0, sizeof(windows));
  focused_handle = 0;
  con_alloc();
  printf("winman: con.enabled=%d surface=%p w=%d h=%d\n", con.enabled,
         (void *)con.surface, con.client_w, con.client_h);
  present_full_desktop();
  printf("winman: ready\n");

  int self_pid = (int)get_pid();
  int tick = 0;
  int32_t last_cx = 0, last_cy = 0;
  int have_last = 0;

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
          present_full_desktop();
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
    if ((tick & 63) == 0)
      reap_dead_windows();

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
      if (have_last) {
        int s = cursor_scale();
        present_rect(last_cx, last_cy, cursor_w() * s, cursor_h() * s);
      }
      int gx, gy, gw, gh;
      compute_ghost((int)mx, (int)my, &gx, &gy, &gw, &gh);
      draw_ghost(gx, gy, gw, gh);
      drag.last_gx = gx;
      drag.last_gy = gy;
      drag.last_gw = gw;
      drag.last_gh = gh;
      drag.have_ghost = 1;

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

    /* ALWAYS erase the old cursor if we have one. If present_dirty()
       already drew over it, this just harmlessly redraws the same pixels. */
    if (have_last) {
      int s = cursor_scale();
      present_rect(last_cx, last_cy, cursor_w() * s, cursor_h() * s);
    }

    draw_cursor(mx, my);
    last_cx = mx;
    last_cy = my;
    have_last = 1;

    yield();
  }
  return 0;
}
