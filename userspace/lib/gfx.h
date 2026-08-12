/* userspace/lib/gfx.h — 2D drawing over a pixel buffer.
 *
 * The primitives winman and its clients both need: rectangles, fills,
 * blits, font8x8 text, mask stamping and BMP drawing, all clipped so a
 * caller can pass coordinates that fall outside the target without
 * checking first. Nothing here allocates or makes syscalls except
 * gfx_sprite_load, which reads a file.
 *
 * Colours are 0xAARRGGBB. Ops that composite (gfx_blend, gfx_fill_blend,
 * gfx_blit_alpha, sprite and BMP drawing) honour the alpha byte;
 * everything else ignores it and writes the low 24 bits, so the existing
 * 0x00RRGGBB palette constants keep working unchanged.
 *
 * Coordinates are pixels, and `stride` is in pixels rather than bytes —
 * a surface over a wm window is (pitch / 4).
 *
 * Implementation: userspace/lib/gfx.c.
 */
#ifndef GFX_H
#define GFX_H

#include "bmp.h"
#include <stdint.h>
#include <stddef.h>

#define GFX_GLYPH_W 8
#define GFX_GLYPH_H 8

/* ---------------- Rectangles -------------------------------------------- */

struct gfx_rect {
    int x, y, w, h;
};

static inline struct gfx_rect gfx_rect_make(int x, int y, int w, int h) {
    struct gfx_rect r = { x, y, w, h };
    return r;
}

static inline int gfx_rect_empty(struct gfx_rect r) {
    return r.w <= 0 || r.h <= 0;
}

static inline int gfx_rect_contains(struct gfx_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* Same size, moved by (dx, dy). */
struct gfx_rect gfx_rect_offset(struct gfx_rect r, int dx, int dy);

/* Overlap of two rectangles. Empty (w or h <= 0) when they miss. */
struct gfx_rect gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b);

/* Smallest rectangle containing both inputs. Empty inputs are ignored, so
 * union(empty, r) == r. */
struct gfx_rect gfx_rect_union(struct gfx_rect a, struct gfx_rect b);

/* Rectangle shrunk by `n` on every side. Useful for insetting a border. */
struct gfx_rect gfx_rect_inset(struct gfx_rect r, int n);

/* ---------------- Surfaces ---------------------------------------------- */

/* A rectangle of pixels someone else owns. `clip` bounds every drawing
 * call; gfx_surface_init sets it to the whole surface. */
struct gfx_surface {
    uint32_t       *px;
    int             w, h;
    int             stride;     /* pixels per row, not bytes */
    struct gfx_rect clip;
};

void gfx_surface_init(struct gfx_surface *s, uint32_t *px,
                      int w, int h, int stride);

/* Restrict drawing to the intersection of the current clip and `r`.
 * Returns the previous clip so a caller can restore it. */
struct gfx_rect gfx_clip_push(struct gfx_surface *s, struct gfx_rect r);
void            gfx_clip_set(struct gfx_surface *s, struct gfx_rect r);
void            gfx_clip_reset(struct gfx_surface *s);

static inline struct gfx_rect gfx_surface_bounds(const struct gfx_surface *s) {
    return gfx_rect_make(0, 0, s->w, s->h);
}

/* ---------------- Pixels and fills -------------------------------------- */

void gfx_pixel(struct gfx_surface *s, int x, int y, uint32_t color);
void gfx_blend(struct gfx_surface *s, int x, int y, uint32_t argb);

void gfx_clear(struct gfx_surface *s, uint32_t color);
void gfx_fill(struct gfx_surface *s, struct gfx_rect r, uint32_t color);
void gfx_fill_blend(struct gfx_surface *s, struct gfx_rect r, uint32_t argb);

void gfx_hline(struct gfx_surface *s, int x, int y, int w, uint32_t color);
void gfx_vline(struct gfx_surface *s, int x, int y, int h, uint32_t color);

/* Outline drawn inside `r`, `thickness` pixels wide. */
void gfx_frame(struct gfx_surface *s, struct gfx_rect r,
               uint32_t color, int thickness);

/* Two-tone bevel: `light` along the top and left, `dark` along the bottom
 * and right. Flip the two arguments for a pressed-in look. */
void gfx_bevel(struct gfx_surface *s, struct gfx_rect r,
               uint32_t light, uint32_t dark, int thickness);

/* ---------------- Blitting ----------------------------------------------- */

/* Copy `src_rect` of `src` to (dx, dy) in `dst`, ignoring alpha. Passing
 * an empty src_rect means the whole source surface. */
void gfx_blit(struct gfx_surface *dst, int dx, int dy,
              const struct gfx_surface *src, struct gfx_rect src_rect);

/* As gfx_blit, compositing each pixel over the destination by its alpha. */
void gfx_blit_alpha(struct gfx_surface *dst, int dx, int dy,
                    const struct gfx_surface *src, struct gfx_rect src_rect);

/* Nearest-neighbour integer magnification. scale <= 1 behaves as 1. */
void gfx_blit_scaled(struct gfx_surface *dst, int dx, int dy,
                     const struct gfx_surface *src, struct gfx_rect src_rect,
                     int scale, int use_alpha);

/* ---------------- Text ---------------------------------------------------- */

/* font8x8 covers 0x20..0x7E; anything else draws as a space. `scale`
 * magnifies by whole pixels, so scale 2 gives a 16x16 cell. */
void gfx_glyph(struct gfx_surface *s, int x, int y, char c,
               uint32_t fg, int scale);
void gfx_glyph_bg(struct gfx_surface *s, int x, int y, char c,
                  uint32_t fg, uint32_t bg, int scale);

/* Draw a NUL-terminated string left to right. No wrapping and no newline
 * handling — a caller wanting either owns the line breaking. */
void gfx_text(struct gfx_surface *s, int x, int y, const char *str,
              uint32_t fg, int scale);
void gfx_text_bg(struct gfx_surface *s, int x, int y, const char *str,
                 uint32_t fg, uint32_t bg, int scale);

/* At most `n` characters, for fixed-width fields that must not overrun. */
void gfx_text_n(struct gfx_surface *s, int x, int y, const char *str,
                size_t n, uint32_t fg, int scale);

/* Pixel size the string would occupy. Either out pointer may be NULL. */
void gfx_text_size(const char *str, int scale, int *out_w, int *out_h);

/* How many characters of `str` fit in `max_w` pixels. */
int  gfx_text_fit(const char *str, int scale, int max_w);

enum gfx_text_align {
    GFX_TEXT_LEFT   = 0,
    GFX_TEXT_CENTER = 1,
    GFX_TEXT_RIGHT  = 2,
};

/* Draw one clipped line inside `box`, fitting whole glyph cells only.
 * Padding is applied on both horizontal sides. Returns the number of
 * characters drawn, which is useful for tests and status widgets. */
int  gfx_text_box(struct gfx_surface *s, struct gfx_rect box,
                  const char *str, uint32_t fg, int scale,
                  int pad, enum gfx_text_align align);

/* As gfx_text_box, but fills the box with `bg` before drawing. */
int  gfx_text_box_bg(struct gfx_surface *s, struct gfx_rect box,
                     const char *str, uint32_t fg, uint32_t bg, int scale,
                     int pad, enum gfx_text_align align);

/* ---------------- Masks and images ---------------------------------------- */

/* Stamp `color` wherever the mask byte is non-zero, leaving the rest of
 * the destination untouched. The mask is `mw` bytes per row. */
void gfx_mask(struct gfx_surface *s, int x, int y,
              const uint8_t *mask, int mw, int mh,
              uint32_t color, int scale);

/* As gfx_mask, but each distinct mask value picks its own colour:
 * colors[v - 1] for v in 1..ncolors. Values past the table are skipped.
 * This is the two-tone scheme the cursor and button glyphs use. */
void gfx_mask_multi(struct gfx_surface *s, int x, int y,
                    const uint8_t *mask, int mw, int mh,
                    const uint32_t *colors, int ncolors, int scale);

void gfx_draw_bmp(struct gfx_surface *s, int x, int y,
                  const struct bmp_image *img, int scale);

/* ---------------- Sprites -------------------------------------------------
 *
 * A sprite is an image loaded from disk with a built-in mask to fall back
 * on. It is the pattern the cursor already uses and the titlebar glyphs
 * are heading for: art is replaceable without a rebuild, but a missing or
 * malformed file degrades to something drawable rather than to nothing.
 */
struct gfx_sprite {
    struct bmp_image image;        /* pixels != 0 once a load succeeds   */
    const uint8_t   *mask;         /* fallback; mh rows of mw bytes      */
    int              mask_w, mask_h;
    uint32_t         colors[3];    /* colour for mask values 1, 2, 3     */
    int              ncolors;
};

/* Point a sprite at its built-in artwork. Call before gfx_sprite_load so
 * a failed load still leaves something drawable. */
void gfx_sprite_set_mask(struct gfx_sprite *sp, const uint8_t *mask,
                         int mw, int mh,
                         const uint32_t *colors, int ncolors);

/* Replace the artwork from a BMP. Returns 0 on success. On failure the
 * sprite is untouched, so the fallback mask stays in effect. */
int  gfx_sprite_load(struct gfx_sprite *sp, const char *path);

void gfx_sprite_free(struct gfx_sprite *sp);

int  gfx_sprite_w(const struct gfx_sprite *sp);
int  gfx_sprite_h(const struct gfx_sprite *sp);

void gfx_sprite_draw(struct gfx_surface *s, int x, int y,
                     const struct gfx_sprite *sp, int scale);

#endif
