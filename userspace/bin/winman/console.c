#define WINMAN_DECLARE_STATE
#include "winman.h"
#include "key_codes.h"
#include "syscall.h"
#include <display/print.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

/* Pre-expanded glyph row for the console (CONSOLE_FG/CONSOLE_BG are
 * constants): byte -> 8 pixels. Built in con_alloc_buffers. 8 KiB BSS.
 * Per-row glyph render becomes a single memcpy(32 B) instead of 8 per-pixel
 * conditional writes , the dominant cost when winman drains the TTY ring. */
static uint32_t con_glyph_lut[256][FONT_GLYPH_W];

void build_con_glyph_lut(void) {
  for (int b = 0; b < 256; b++) {
    for (int c = 0; c < FONT_GLYPH_W; c++) {
      con_glyph_lut[b][c] = ((b >> c) & 1) ? CONSOLE_FG : CONSOLE_BG;
    }
  }
}

/* Fill `n` 32-bit words at `dst` with `color`. Writes in 64-bit pairs when
 * the destination is 8-aligned, falls back to single stores at the tail.
 * Used for big solid rectangles and console scroll fills. */
void fill_dwords(uint32_t *dst, size_t n, uint32_t color) {
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


static struct ttf_font con_font;
static int con_ttf_ready = 0;
static int con_ttf_cell_w = 9;
static int con_ttf_cell_h = 18;
static int con_ttf_ascent = 12;
static int con_ttf_descent = -3;

int con_cell_w_for_scale(int scale) {
  if (scale < 1)
    scale = 1;
  return (con_ttf_ready ? con_ttf_cell_w : FONT_GLYPH_W) * scale;
}

int con_cell_h_for_scale(int scale) {
  if (scale < 1)
    scale = 1;
  return (con_ttf_ready ? con_ttf_cell_h : FONT_GLYPH_H) * scale;
}

/* Cell metrics are per-console: each one carries its own zoom level, so
 * Ctrl+= in one shell does not resize the others. The font itself is shared
 * , con_font/con_ttf_* are loaded once and describe the face, not a size. */
int con_cell_w(const struct console *c) {
  return con_cell_w_for_scale(c->scale);
}
int con_cell_h(const struct console *c) {
  return con_cell_h_for_scale(c->scale);
}

int con_font_px(const struct console *c) {
  int scale = c->scale < 1 ? 1 : c->scale;
  return CON_TTF_PX * scale;
}

/* Finished cell images (glyph + background) for every printable ASCII,
 * one cache per zoom level, so drawing a cell is row memcpys -- the same
 * shape as the font8x8 LUT. Built lazily: a console that never zooms
 * past 1 never pays for the bigger scales. */
static uint32_t *ttf_cell_cache[CON_SCALE_MAX + 1];

static void ttf_cache_build(int scale) {
  if (scale < CON_SCALE_MIN || scale > CON_SCALE_MAX || !con_ttf_ready)
    return;
  if (ttf_cell_cache[scale])
    return;

  int cw = con_cell_w_for_scale(scale);
  int chh = con_cell_h_for_scale(scale);
  size_t cell_px = (size_t)cw * (size_t)chh;
  uint32_t *mem = malloc(95 * cell_px * 4);
  if (!mem)
    return;

  for (int c = 32; c <= 126; c++) {
    uint32_t *cell = mem + (size_t)(c - 32) * cell_px;
    fill_dwords(cell, cell_px, CONSOLE_BG);
    if (c == ' ')
      continue;

    /* Raster with the same math con_draw_glyph uses, at cell origin
     * (0,0) -- translation invariance is what makes the cache valid. */
    struct gfx_surface s;
    gfx_surface_init(&s, cell, cw, chh, cw);
    struct gfx_rect prev = gfx_clip_push(&s, gfx_rect_make(0, 0, cw, chh));
    int band = (con_ttf_ascent - con_ttf_descent) * scale;
    int baseline = (chh - band) / 2 + con_ttf_ascent * scale;
    ttf_draw_glyph_cell(&s, &con_font, 0, baseline, cw, c,
                        CON_TTF_PX * scale, CONSOLE_FG);
    gfx_clip_set(&s, prev);
  }
  ttf_cell_cache[scale] = mem;
}

/* Consoles are addressed by handle everywhere outside this file's console
 * code, because that is what focus, z-order and hit-testing deal in. */
int is_console_handle(int handle) {
  return handle >= HANDLE_CONSOLE_BASE &&
         handle < HANDLE_CONSOLE_BASE + CON_MAX;
}

struct console *con_for_handle(int handle) {
  if (!is_console_handle(handle))
    return 0;
  struct console *c = &cons[handle - HANDLE_CONSOLE_BASE];
  return c->win.in_use ? c : 0;
}

/* The console that currently has focus, or NULL when focus is on a client
 * window or nothing. Keystrokes and console zoom go here. */
struct console *con_focused(void) { return con_for_handle(focused_handle); }

void con_try_load_ttf(void) {
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


/* Console resize: reallocate surface + backing buffer at new dims and copy
 * the still-visible region of the old surface into the new one so existing
 * glyphs survive. Cursor is clamped to the new grid. */
void console_resize(struct console *c, int new_cw, int new_ch) {
  if (!c || !c->win.in_use)
    return;
  /* Snap to glyph grid so cells line up without trailing fractional row. */
  int cell_w = con_cell_w(c);
  int cell_h = con_cell_h(c);
  new_cw = (new_cw / cell_w) * cell_w;
  new_ch = (new_ch / cell_h) * cell_h;
  if (new_cw < MIN_CLIENT_W)
    new_cw = MIN_CLIENT_W;
  if (new_ch < MIN_CLIENT_H)
    new_ch = MIN_CLIENT_H;
  if (new_cw == c->win.client_w && new_ch == c->win.client_h)
    return;

  size_t pixel_bytes = (size_t)new_cw * (size_t)new_ch * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *new_raw = 0;
  uint32_t *new_surf = (uint32_t *)page_aligned_alloc(pages, &new_raw);
  if (!new_surf)
    return;
  fill_dwords(new_surf, (size_t)new_cw * (size_t)new_ch, CONSOLE_BG);

  void *new_back_raw = 0;
  uint32_t *new_back = (uint32_t *)page_aligned_alloc(pages, &new_back_raw);
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
  if (c->cells) {
    int copy_cols = c->cols < new_cols ? c->cols : new_cols;
    int copy_rows = c->rows < new_rows ? c->rows : new_rows;
    for (int y = 0; y < copy_rows; y++) {
      memcpy(new_cells + y * new_cols, c->cells + y * c->cols,
             (size_t)copy_cols);
    }
  }

  if (c->win.surface_raw)
    free(c->win.surface_raw);
  if (c->backing_raw)
    free(c->backing_raw);
  if (c->cells)
    free(c->cells);
  if (c->saved_cells)
    free(c->saved_cells);

  c->win.surface = new_surf;
  c->win.surface_raw = new_raw;
  c->cells = new_cells;
  c->backing = new_back;
  c->backing_raw = new_back_raw;
  c->saved_cells = new_saved;
  c->win.client_w = new_cw;
  c->win.client_h = new_ch;
  c->cols = new_cols;
  c->rows = new_rows;
  if (c->cx >= c->cols)
    c->cx = c->cols - 1;
  if (c->cy >= c->rows)
    c->cy = c->rows - 1;
  c->saved_valid = 0;
  con_redraw(c);
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

void con_draw_glyph(struct console *con, int gx, int gy, char c) {
  int cell_w = con_cell_w(con);
  int cell_h = con_cell_h(con);
  int px = gx * cell_w;
  int py = gy * cell_h;
  if (px + cell_w > con->win.client_w)
    return;
  if (py + cell_h > con->win.client_h)
    return;

  if (con_ttf_ready) {
    unsigned ch = (unsigned char)c;
    int scale = con->scale;

    /* Fast path. A cached cell already carries its background, so a hit
     * is just row copies -- no fill, no raster. Guarded by an explicit
     * range check rather than a clamped index: cell geometry is a pure
     * function of scale, so a clamped index against unclamped geometry
     * could mismatch the cache's cell size. con_set_scale keeps scale in
     * range; this makes that invariant load-bearing instead of assumed. */
    if (scale >= CON_SCALE_MIN && scale <= CON_SCALE_MAX && ch >= 32 &&
        ch <= 126 && c != ' ') {
      if (!ttf_cell_cache[scale])
        ttf_cache_build(scale);
      const uint32_t *cache = ttf_cell_cache[scale];
      if (cache) {
        size_t cell_px = (size_t)cell_w * (size_t)cell_h;
        const uint32_t *src = cache + (size_t)(ch - 32) * cell_px;
        uint32_t *dst = con->win.surface +
                        (size_t)py * (size_t)con->win.client_w + (size_t)px;
        for (int y = 0; y < cell_h; y++) {
          memcpy(dst, src, (size_t)cell_w * 4);
          dst += con->win.client_w;
          src += cell_w;
        }
        return;
      }
    }

    /* Slow path: build failed, or a blank/non-printable cell. Clear the
     * background, then raster directly for real glyphs -- unchanged from
     * the original, kept as a live fallback rather than deleted. */
    for (int y = 0; y < cell_h; y++) {
      uint32_t *line = con->win.surface +
                       (size_t)(py + y) * (size_t)con->win.client_w +
                       (size_t)px;
      fill_dwords(line, (size_t)cell_w, CONSOLE_BG);
    }
    if (ch < 32 || ch > 126 || c == ' ')
      return;

    struct gfx_surface s;
    gfx_surface_init(&s, con->win.surface, con->win.client_w,
                     con->win.client_h, con->win.client_w);
    struct gfx_rect prev =
        gfx_clip_push(&s, gfx_rect_make(px, py, cell_w, cell_h));

    /* Sit the baseline so the ascender/descender band is centred in the cell
     * instead of guessing at 3/4 of the way down. */
    int bscale = scale < 1 ? 1 : scale;
    int band = (con_ttf_ascent - con_ttf_descent) * bscale;
    int baseline = py + (cell_h - band) / 2 + con_ttf_ascent * bscale;

    ttf_draw_glyph_cell(&s, &con_font, px, baseline, cell_w, ch,
                        con_font_px(con), CONSOLE_FG);
    gfx_clip_set(&s, prev);
    return;
  }

  const uint8_t *glyph;
  static const uint8_t blank[FONT_GLYPH_H] = {0};
  if (c < FONT_FIRST || c > FONT_LAST)
    glyph = blank;
  else
    glyph = font8x8[(int)c - FONT_FIRST];

  if (con->scale == 1) {
    uint32_t *line =
        con->win.surface + (size_t)py * (size_t)con->win.client_w + (size_t)px;
    for (int r = 0; r < FONT_GLYPH_H; r++) {
      memcpy(line, con_glyph_lut[glyph[r]], FONT_GLYPH_W * sizeof(uint32_t));
      line += con->win.client_w;
    }
    return;
  }

  for (int r = 0; r < FONT_GLYPH_H; r++) {
    for (int sy = 0; sy < con->scale; sy++) {
      int y = py + r * con->scale + sy;
      uint32_t *line =
          con->win.surface + (size_t)y * (size_t)con->win.client_w + (size_t)px;
      for (int col = 0; col < FONT_GLYPH_W; col++) {
        uint32_t color = ((glyph[r] >> col) & 1) ? CONSOLE_FG : CONSOLE_BG;
        for (int sx = 0; sx < con->scale; sx++)
          line[col * con->scale + sx] = color;
      }
    }
  }
}

void con_redraw(struct console *con) {
  if (!con || !con->win.in_use || !con->win.surface || !con->cells)
    return;
  fill_dwords(con->win.surface,
              (size_t)con->win.client_w * (size_t)con->win.client_h,
              CONSOLE_BG);
  for (int y = 0; y < con->rows; y++) {
    for (int x = 0; x < con->cols; x++) {
      char c = con->cells[y * con->cols + x];
      if (c)
        con_draw_glyph(con, x, y, c);
    }
  }
  mark_dirty(con->win.x, con->win.y, outer_w_dims(con->win.client_w),
             outer_h_dims(con->win.client_h, con->win.status_h));
}

int con_set_scale(struct console *con, int new_scale) {
  if (!con || !con->win.in_use)
    return -1;
  if (new_scale < CON_SCALE_MIN)
    new_scale = CON_SCALE_MIN;
  if (new_scale > CON_SCALE_MAX)
    new_scale = CON_SCALE_MAX;
  if (new_scale == con->scale)
    return con->scale;

  int new_cols = con->win.client_w / con_cell_w_for_scale(new_scale);
  int new_rows = con->win.client_h / con_cell_h_for_scale(new_scale);
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

  int copy_cols = con->cols < new_cols ? con->cols : new_cols;
  int copy_rows = con->rows < new_rows ? con->rows : new_rows;
  int src_y0 = con->cy >= copy_rows ? con->cy - copy_rows + 1 : 0;
  int dst_y0 = 0;
  for (int y = 0; y < copy_rows; y++) {
    memcpy(new_cells + (dst_y0 + y) * new_cols,
           con->cells + (src_y0 + y) * con->cols, (size_t)copy_cols);
  }

  int new_cx = con->cx < new_cols ? con->cx : new_cols - 1;
  int new_cy = con->cy - src_y0 + dst_y0;
  if (new_cy < 0)
    new_cy = 0;
  if (new_cy >= new_rows)
    new_cy = new_rows - 1;

  free(con->cells);
  if (con->saved_cells)
    free(con->saved_cells);
  con->cells = new_cells;
  con->saved_cells = new_saved;
  con->cols = new_cols;
  con->rows = new_rows;
  con->cx = new_cx;
  con->cy = new_cy;
  con->scale = new_scale;
  con->saved_valid = 0;
  con_redraw(con);
  return con->scale;
}

/* Scroll one cell row. The cells grid moves up and the pixels move with
 * it: a cell's image depends only on its glyph and grid position, so
 * shifting the surface up by cell_h lines reproduces exactly what a full
 * con_redraw would paint -- without re-rasterising ~5000 glyphs. */
void con_scroll(struct console *con) {
  if (con->rows <= 1)
    return;
  if (!con->win.surface || !con->cells)
    return;

  memmove(con->cells, con->cells + con->cols,
          (size_t)(con->rows - 1) * (size_t)con->cols);
  memset(con->cells + (con->rows - 1) * con->cols, 0, (size_t)con->cols);

  size_t pitch = (size_t)con->win.client_w;
  size_t row_lines = (size_t)con_cell_h(con);
  size_t move_lines = (size_t)(con->rows - 1) * row_lines;

  memmove(con->win.surface, con->win.surface + row_lines * pitch,
          move_lines * pitch * sizeof(uint32_t));
  fill_dwords(con->win.surface + move_lines * pitch, pitch * row_lines,
              CONSOLE_BG);

  /* Chrome is unchanged and the backbuffer still holds valid chrome
   * pixels, so damage only the client area that shifted. */
  mark_dirty(con->win.x + BORDER_PX, con->win.y + TITLEBAR_PX,
             con->win.client_w, con->win.client_h);
}

void con_newline(struct console *con) {
  con->cx = 0;
  con->cy++;
  if (con->cy >= con->rows) {
    con_scroll(con);
    con->cy = con->rows - 1;
  }
}

/* Wipe the live console surface back to CONSOLE_BG and home the cursor.
 * Used by both TTY_CTRL_CLEAR and the entry path of TTY_CTRL_PUSH. */
void con_wipe(struct console *con) {
  if (!con->win.in_use || !con->win.surface)
    return;
  fill_dwords(con->win.surface,
              (size_t)con->win.client_w * (size_t)con->win.client_h,
              CONSOLE_BG);
  if (con->cells)
    memset(con->cells, 0, (size_t)con->cols * (size_t)con->rows);
  con->cx = con->cy = 0;
  mark_dirty(con->win.x, con->win.y, outer_w_dims(con->win.client_w),
             outer_h_dims(con->win.client_h, con->win.status_h));
}

void con_save(struct console *con) {
  if (!con->win.in_use || !con->win.surface || !con->backing)
    return;
  size_t pixels = (size_t)con->win.client_w * (size_t)con->win.client_h;
  memcpy(con->backing, con->win.surface, pixels * 4);
  if (con->cells && con->saved_cells)
    memcpy(con->saved_cells, con->cells, (size_t)con->cols * (size_t)con->rows);
  con->saved_cx = con->cx;
  con->saved_cy = con->cy;
  con->saved_valid = 1;
}

void con_restore(struct console *con) {
  if (!con->win.in_use || !con->win.surface || !con->backing ||
      !con->saved_valid)
    return;
  size_t pixels = (size_t)con->win.client_w * (size_t)con->win.client_h;
  memcpy(con->win.surface, con->backing, pixels * 4);
  if (con->cells && con->saved_cells)
    memcpy(con->cells, con->saved_cells, (size_t)con->cols * (size_t)con->rows);
  con->cx = con->saved_cx;
  con->cy = con->saved_cy;
  con->saved_valid = 0;
  mark_dirty(con->win.x, con->win.y, outer_w_dims(con->win.client_w),
             outer_h_dims(con->win.client_h, con->win.status_h));
}

void con_putc(struct console *con, char c) {
  if (!con->win.in_use)
    return;

  /* Control characters (newline, scroll, clear, etc.) call con_redraw/con_wipe,
   * which already mark the whole console dirty. We only need to mark the
   * specific cell for standard printable characters. */
  if (c == '\n') {
    con_newline(con);
    return;
  }
  if (c == '\r') {
    con->cx = 0;
    return;
  }
  if (c == '\b') {
    if (con->cx > 0) {
      con->cx--;
      con->cells[con->cy * con->cols + con->cx] = 0;
      con_draw_glyph(con, con->cx, con->cy, ' ');

      /* Mark only the erased cell dirty */
      int cell_x = con->win.x + BORDER_PX + con->cx * con_cell_w(con);
      int cell_y = con->win.y + TITLEBAR_PX + con->cy * con_cell_h(con);
      mark_dirty(cell_x, cell_y, con_cell_w(con), con_cell_h(con));
    }
    return;
  }
  if (c == '\t') {
    do {
      con_putc(con, ' ');
    } while (con->cx % 8);
    return;
  }
  if (c == TTY_CTRL_CLEAR) {
    con_wipe(con);
    return;
  }
  if (c == TTY_CTRL_PUSH) {
    con_save(con);
    con_wipe(con);
    return;
  }
  if (c == TTY_CTRL_POP) {
    con_restore(con);
    return;
  }
  if (c == TTY_CTRL_ZOOM_IN) {
    con_set_scale(con, con->scale + 1);
    return;
  }
  if (c == TTY_CTRL_ZOOM_OUT) {
    con_set_scale(con, con->scale - 1);
    return;
  }

  if (con->cx >= con->cols)
    con_newline(con);

  con->cells[con->cy * con->cols + con->cx] = c;
  con_draw_glyph(con, con->cx, con->cy, c);

  int cell_x = con->win.x + BORDER_PX + con->cx * con_cell_w(con);
  int cell_y = con->win.y + TITLEBAR_PX + con->cy * con_cell_h(con);
  mark_dirty(cell_x, cell_y, con_cell_w(con), con_cell_h(con));

  con->cx++;
}

/* Where console `slot` opens. Slot 0 takes the whole desktop inset by a
 * margin, which is what the one built-in console always did. Later slots
 * cascade down-right and shrink to match, so a second shell never opens
 * exactly on top of the first and both stay fully on screen. */
void console_geometry(int slot, int *out_x, int *out_y, int *out_cw,
                             int *out_ch) {
  int margin = 16;
  int step = slot * (TITLEBAR_PX + 8);
  *out_x = margin + step;
  *out_y = margin + step;
  *out_cw = fb_w - 2 * margin - 2 * BORDER_PX - step;
  *out_ch = fb_h - 2 * margin - TITLEBAR_PX - BORDER_PX - TASKBAR_PX - step;
}

/* Surface, alt-screen backing buffer and cell grid for one console. All four
 * allocations succeed together or none of them do: a console holding two of
 * the four would draw into a grid that does not match its surface. */
int con_alloc_buffers(struct console *con, int slot) {
  int x, y, cw, ch;
  console_geometry(slot, &x, &y, &cw, &ch);
  if (cw < 64 || ch < 64)
    return -1;

  con_try_load_ttf();
  con->scale = CON_SCALE_MIN;
  int cell_w = con_cell_w(con);
  int cell_h = con_cell_h(con);
  cw = (cw / cell_w) * cell_w;
  ch = (ch / cell_h) * cell_h;

  size_t pixel_bytes = (size_t)cw * (size_t)ch * 4;
  size_t pages = (pixel_bytes + 4095) / 4096;

  void *surface_raw = 0, *backing_raw = 0;
  uint32_t *surface = (uint32_t *)page_aligned_alloc(pages, &surface_raw);
  uint32_t *backing = (uint32_t *)page_aligned_alloc(pages, &backing_raw);
  int cols = cw / cell_w;
  int rows = ch / cell_h;
  char *cells = (char *)malloc((size_t)cols * (size_t)rows);
  char *saved_cells = (char *)malloc((size_t)cols * (size_t)rows);

  if (!surface || !backing || !cells || !saved_cells) {
    if (surface_raw)
      free(surface_raw);
    if (backing_raw)
      free(backing_raw);
    if (cells)
      free(cells);
    if (saved_cells)
      free(saved_cells);
    return -1;
  }

  fill_dwords(surface, (size_t)cw * (size_t)ch, CONSOLE_BG);
  memset(cells, 0, (size_t)cols * (size_t)rows);
  memset(saved_cells, 0, (size_t)cols * (size_t)rows);
  build_con_glyph_lut();

  con->win.surface = surface;
  con->win.surface_raw = surface_raw;
  con->backing = backing;
  con->backing_raw = backing_raw;
  con->cells = cells;
  con->saved_cells = saved_cells;
  con->win.client_w = cw;
  con->win.client_h = ch;
  con->win.x = x;
  con->win.y = y;
  con->cols = cols;
  con->rows = rows;
  con->cx = con->cy = 0;
  con->saved_valid = 0;
  return 0;
}

void con_set_title(struct console *con, int slot) {
  /* "Console", "Console 2", "Console 3" , the first one keeps the bare name
   * it has always had, so the common single-console desktop is unchanged. */
  char buf[16];
  int n = 0;
  const char *base = "Console";
  while (base[n]) {
    buf[n] = base[n];
    n++;
  }
  if (slot > 0) {
    buf[n++] = ' ';
    buf[n++] = (char)('1' + slot);
  }
  buf[n] = 0;

  size_t i = 0;
  while (buf[i] && i < sizeof(con->win.title) - 1) {
    con->win.title[i] = buf[i];
    i++;
  }
  con->win.title[i] = 0;
}

/* Open a console window with a shell running on its own TTY channel.
 * Returns NULL when every slot is taken, when a channel cannot be claimed,
 * or when the shell fails to start , in which case nothing is left behind.
 *
 * Slot 0 is the exception at the channel level only: it mirrors TTY_KERNEL,
 * which the kernel opened at boot and never closes, so there is nothing to
 * allocate or release for it. It still gets its own shell from here, which
 * is what makes closing it possible , winman knows the pid. */
struct console *console_open(void) {
  int slot = -1;
  for (int i = 0; i < CON_MAX; i++) {
    if (!cons[i].win.in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    printf("winman: console open refused , all %d slots in use\n", CON_MAX);
    return 0;
  }

  struct console *con = &cons[slot];
  memset(con, 0, sizeof(*con));

  int tty = 0;
  if (slot > 0) {
    long claimed = tty_alloc();
    if (claimed < 0) {
      printf("winman: console open failed , no free TTY channel\n");
      return 0;
    }
    tty = (int)claimed;
  }

  if (con_alloc_buffers(con, slot) != 0) {
    printf("winman: console open failed , surface alloc\n");
    if (slot > 0)
      tty_free(tty);
    memset(con, 0, sizeof(*con));
    return 0;
  }

  con->tty = tty;
  con->win.handle = HANDLE_CONSOLE_BASE + slot;
  con_set_title(con, slot);
  con->win.in_use = 1;

  char *argv[] = {(char *)"sh", 0};
  long pid = tty_spawn("/system/bin/sh.elf", argv, tty);
  if (pid <= 0) {
    printf("winman: console open failed , sh spawn returned %ld\n", pid);
    con->win.in_use = 0;
    free(con->win.surface_raw);
    free(con->backing_raw);
    free(con->cells);
    free(con->saved_cells);
    if (slot > 0)
      tty_free(tty);
    memset(con, 0, sizeof(*con));
    return 0;
  }
  con->pid = (int)pid;

  z_bring_to_front(con->win.handle);
  focused_handle = con->win.handle;
  mark_dirty(con->win.x, con->win.y, outer_w_dims(con->win.client_w),
             outer_h_dims(con->win.client_h, con->win.status_h));
  printf("winman: console slot=%d tty=%d sh pid=%d\n", slot, tty, con->pid);
  return con;
}

/* Tear a console down: kill its shell, give the channel back, free the
 * buffers, drop the window. Safe to call for a console whose shell is
 * already gone , that is how the reaper closes one the user exited out of. */
void console_close(struct console *con) {
  if (!con || !con->win.in_use)
    return;

  int slot = (int)(con - cons);
  int handle = con->win.handle;
  int old_x = con->win.x, old_y = con->win.y;
  int old_ow = outer_w_dims(con->win.client_w);
  int old_oh = outer_h_dims(con->win.client_h, con->win.status_h);

  if (con->pid > 0)
    kill(con->pid, 9);
  /* Channel 0 belongs to the kernel; the others were ours to hand back. */
  if (con->tty > 0)
    tty_free(con->tty);

  if (con->win.surface_raw)
    free(con->win.surface_raw);
  if (con->backing_raw)
    free(con->backing_raw);
  if (con->cells)
    free(con->cells);
  if (con->saved_cells)
    free(con->saved_cells);

  memset(con, 0, sizeof(*con));
  z_remove(handle);

  if (close_pending_handle == handle)
    close_pending_handle = -1;

  if (focused_handle == handle) {
    focused_handle = z_count > 0 ? z_order[0] : 0;
    mark_dirty(0, taskbar_y(), fb_w, TASKBAR_PX);
  }

  mark_dirty(old_x, old_y, old_ow, old_oh);
  printf("winman: console slot=%d closed\n", slot);
}

/* True for the shell binary, whatever directory it was found in. */
int path_is_shell(const char *path) {
  if (!path)
    return 0;
  const char *base = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\')
      base = p + 1;
  }
  return strcmp(base, "sh.elf") == 0;
}

/* Launch from the desktop or the start menu. A shell is not an ordinary
 * spawn: without a console of its own it inherits winman's TTY channel,
 * so its output lands in whichever console winman is attached to and it
 * never receives a keystroke. Give it a window and a channel instead. */
void launch_program(const char *path) {
  if (path_is_shell(path)) {
    console_open();
    return;
  }

  char *argv[] = {(char *)path, 0};
  long pid = spawn(path, argv);
  if (pid > 0)
    printf("winman: spawned %s pid %ld\n", path, pid);
  else
    printf("winman: failed to spawn %s (code %ld)\n", path, pid);
}
