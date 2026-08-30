#define WINMAN_DECLARE_STATE
#include "winman.h"
#include "key_codes.h"
#include "syscall.h"
#include <display/print.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

int outer_w(const struct window *w) {
  return w->client_w + 2 * BORDER_PX;
}
int outer_h(const struct window *w) {
  return w->client_h + TITLEBAR_PX + BORDER_PX + w->status_h;
}

/* Keep Winman call sites terse while the reusable library owns clipping and
 * union semantics. */
void mark_dirty(int x, int y, int w, int h) {
  gfx_damage_add(&desktop_damage, gfx_rect_make(x, y, w, h),
                 gfx_rect_make(0, 0, fb_w, fb_h));
}

/* Missing artwork falls back to the built-in mask. */
static struct bmp_image tb_start_icon;
static int tb_start_icon_loaded = 0;
void tb_load_start_icon(void) {
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


/* Grow geometrically and retain the allocation across host resizes. Width-only
 * changes normally need no growth because virtio keeps a stable row pitch. */
int backbuffer_reserve(size_t required) {
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
int backbuffer_register(void) {
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

uint32_t *fb_pix(int x, int y) {
  return fb + (size_t)y * (size_t)fb_stride + (size_t)x;
}

/* Compose clip: the region present_dirty() is about to copy out to the
 * hardware framebuffer. Every compositor primitive must honour this clip:
 * the backbuffer persists between frames, so an out-of-clip write is not
 * harmless. Cursor and drag repairs may expose it during a later present. */
static int clip_x, clip_y, clip_w, clip_h;

void clip_set(int x, int y, int w, int h) {
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
int clip_hits(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0 || clip_w <= 0 || clip_h <= 0)
    return 0;
  return x < clip_x + clip_w && x + w > clip_x && y < clip_y + clip_h &&
         y + h > clip_y;
}

int clip_contains_point(int x, int y) {
  return clip_w > 0 && clip_h > 0 && x >= clip_x && y >= clip_y &&
         x < clip_x + clip_w && y < clip_y + clip_h;
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
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

void draw_glyph_fb(int x, int y, char c, uint32_t fg, uint32_t bg) {
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

void draw_text_fb(int x, int y, const char *s, int max_w, uint32_t fg,
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
void titlebar_btn_rect(int win_x, int win_y, int outer_w,
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
void draw_button_mask(int x, int y,
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

void draw_chrome(const struct window *w, int focused) {
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

void blit_surface(const struct window *w) {
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

void blit_icon(int dst_x, int dst_y, int dst_w, int dst_h, int src_w,
                      int src_h, uint32_t *pixels);

/* Scale available artwork inside the padded button. */
void draw_start_button(int y) {
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

void clock_format(void) {
  struct calendar_time now;
  time_to_calendar(time(0), &now);

  /* 12-hour with AM/PM, matching the taskbar it is modelled on. Hour 0 is
   * 12 AM and hour 12 is 12 PM , the modulo alone gets both wrong. */
  int hour12 = now.hour % 12;
  if (hour12 == 0)
    hour12 = 12;

  snprintf(clock_time_text, sizeof(clock_time_text), "%d:%02d %s", hour12,
           now.minute, now.hour < 12 ? "AM" : "PM");
  snprintf(clock_date_text, sizeof(clock_date_text), "%02d/%02d/%04d", now.day,
           now.month, now.year);
}

int clock_rect(int *cx, int *cy, int *cw, int *ch) {
  *cw = CLOCK_W;
  *ch = TASKBAR_PX;
  *cx = fb_w - CLOCK_W - CLOCK_PAD_R;
  *cy = taskbar_y();
  return *cx > TASKBAR_START_W && *cy >= 0;
}

/* Reformat and damage the clock when the displayed text actually changes.
 * Returns 1 if a repaint was requested. */
int clock_tick(void) {
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

void draw_clock(void) {
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

void draw_taskbar(void) {
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

  struct tb_entry ents[MAX_Z];
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


void draw_start_menu(void) {
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


void drain_tty_into_console(void) {
  char buf[256];
  for (int i = 0; i < CON_MAX; i++) {
    struct console *con = &cons[i];
    if (!con->win.in_use)
      continue;
    long n;
    while ((n = tty_drain(con->tty, buf, sizeof(buf))) > 0) {
      for (long j = 0; j < n; j++)
        con_putc(con, buf[j]);
      if (n < (long)sizeof(buf))
        break;
    }
  }
}

/* Blit and scale an icon to dst_w x dst_h, supporting alpha and magenta chroma
 * key. */
void blit_icon(int dst_x, int dst_y, int dst_w, int dst_h, int src_w,
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
 * Minimized windows are skipped entirely , they keep their slot + taskbar
 * entry but contribute no pixels until restored. */
void compose_handle(int handle) {
  if (is_minimized(handle))
    return;
  if (is_console_handle(handle)) {
    struct console *con = con_for_handle(handle);
    if (!con)
      return;
    if (!clip_hits(con->win.x, con->win.y, outer_w_dims(con->win.client_w),
                   outer_h_dims(con->win.client_h, con->win.status_h)))
      return;
    struct window cw = {0};
    cw.x = con->win.x;
    cw.y = con->win.y;
    cw.client_w = con->win.client_w;
    cw.client_h = con->win.client_h;
    cw.surface = con->win.surface;
    memcpy(cw.title, con->win.title, sizeof(cw.title) - 1);
    draw_chrome(&cw, focused_handle == handle);
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

void compose(void) {
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
  /* Last, so the dialog sits above the taskbar and the start menu too ,
   * anything it did not cover would be clickable behind a modal. */
  draw_prompt();
}

/* Hand normal backbuffer copies to the kernel so large regions can use its AP
 * work queue. Keep the direct path as a fallback for an older kernel or a
 * rejected request. */
void present_backbuffer_rects(const struct fb_rect *rects,
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

void present_backbuffer_rect(int x, int y, int w, int h) {
  const struct fb_rect rect = {
      .x = (uint32_t)x,
      .y = (uint32_t)y,
      .w = (uint32_t)w,
      .h = (uint32_t)h,
  };
  present_backbuffer_rects(&rect, 1);
}

void present_full_desktop(void) {
  if (!fb_hw || !fb || fb_bytes == 0)
    return;
  clip_set(0, 0, fb_w, fb_h);
  compose();
  present_backbuffer_rect(0, 0, fb_w, fb_h);
  gfx_damage_clear(&desktop_damage);
}

void present_dirty(void) {
  if (!fb_hw || !fb || fb_bytes == 0 ||
      !gfx_damage_pending(&desktop_damage))
    return;

  struct gfx_rect damage = gfx_damage_take(&desktop_damage);

  // Composite only the region we are about to copy out. Everything else
  // would be discarded, and at 1280x800 redrawing the whole desktop for a
  // small dirty box was costing ~6x winman's loop rate.
  clip_set(damage.x, damage.y, damage.w, damage.h);
  compose();

  present_backbuffer_rect(damage.x, damage.y, damage.w, damage.h);
}

int cursor_scale(void) {
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
void blend_px(uint32_t *dst, uint32_t src) {
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

int cursor_rect(int32_t x, int32_t y, int scale, struct fb_rect *rect) {
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

void draw_cursor_with_repairs(int32_t x, int32_t y,
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

void draw_cursor(int32_t x, int32_t y) {
  draw_cursor_with_repairs(x, y, 0, 0);
}

void present_cursor_repair_at(int32_t x, int32_t y) {
  struct fb_rect rect;
  if (cursor_rect(x, y, cursor_scale(), &rect))
    present_backbuffer_rects(&rect, 1);
}

void present_rect(int x, int y, int w, int h) {
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
void erase_ghost(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  present_rect(x, y, w, 1);         /* top    */
  present_rect(x, y + h - 1, w, 1); /* bottom */
  present_rect(x, y, 1, h);         /* left   */
  present_rect(x + w - 1, y, 1, h); /* right  */
}

/* Draw directly to hardware without changing the backbuffer. */
void draw_ghost(int x, int y, int w, int h) {
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
