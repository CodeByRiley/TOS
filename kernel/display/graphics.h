/* kernel/display/graphics.h - 2D drawing over a pixel buffer.
 *
 * Kernel-side mirror of userspace/lib/gfx.h for code that needs the same
 * clipped rectangle, blit, mask, and font8x8 text primitives inside the
 * kernel. It intentionally works over caller-owned memory rather than
 * reaching into framebuffer.c directly; callers that draw into the live
 * framebuffer still own damage marking.
 *
 * Colors are 0xAARRGGBB. Ops that composite honor the alpha byte; direct
 * writes ignore alpha and store the low 24 bits, matching userspace gfx.
 *
 * The only deliberate difference from userspace is file-backed BMP loading:
 * the kernel interface accepts already-decoded ARGB images instead of
 * importing the userspace BMP decoder and allocation policy.
 *
 * Implementation: kernel/display/graphics.c.
 */
#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "utilities/types.h"
#include <stddef.h>
#include <stdint.h>

#define GFX_GLYPH_W 8
#define GFX_GLYPH_H 8

/* Rectangles */

struct gfx_rect {
    int x, y, w, h;
};

SINLINE struct gfx_rect gfx_rect_make(int x, int y, int w, int h) {
    struct gfx_rect r = { x, y, w, h };
    return r;
}

SINLINE int gfx_rect_empty(struct gfx_rect r) {
    return r.w <= 0 || r.h <= 0;
}

SINLINE int gfx_rect_contains(struct gfx_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct gfx_rect gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b);
struct gfx_rect gfx_rect_inset(struct gfx_rect r, int n);

/* Surfaces */

struct gfx_surface {
    uint32_t       *px;
    int             w, h;
    int             stride;     /* pixels per row, not bytes */
    struct gfx_rect clip;
};

void gfx_surface_init(struct gfx_surface *s, uint32_t *px,
                      int w, int h, int stride);

struct gfx_rect gfx_clip_push(struct gfx_surface *s, struct gfx_rect r);
void            gfx_clip_set(struct gfx_surface *s, struct gfx_rect r);
void            gfx_clip_reset(struct gfx_surface *s);

SINLINE struct gfx_rect gfx_surface_bounds(const struct gfx_surface *s) {
    return gfx_rect_make(0, 0, s->w, s->h);
}

/* Pixels and fills */

void gfx_pixel(struct gfx_surface *s, int x, int y, uint32_t color);
void gfx_blend(struct gfx_surface *s, int x, int y, uint32_t argb);

void gfx_clear(struct gfx_surface *s, uint32_t color);
void gfx_fill(struct gfx_surface *s, struct gfx_rect r, uint32_t color);
void gfx_fill_blend(struct gfx_surface *s, struct gfx_rect r, uint32_t argb);

void gfx_hline(struct gfx_surface *s, int x, int y, int w, uint32_t color);
void gfx_vline(struct gfx_surface *s, int x, int y, int h, uint32_t color);

void gfx_frame(struct gfx_surface *s, struct gfx_rect r,
               uint32_t color, int thickness);
void gfx_bevel(struct gfx_surface *s, struct gfx_rect r,
               uint32_t light, uint32_t dark, int thickness);

/* Blitting */

void gfx_blit(struct gfx_surface *dst, int dx, int dy,
              const struct gfx_surface *src, struct gfx_rect src_rect);
void gfx_blit_alpha(struct gfx_surface *dst, int dx, int dy,
                    const struct gfx_surface *src, struct gfx_rect src_rect);
void gfx_blit_scaled(struct gfx_surface *dst, int dx, int dy,
                     const struct gfx_surface *src, struct gfx_rect src_rect,
                     int scale, int use_alpha);

/* ---------------- Text --------------------------------------------------- */

void gfx_glyph(struct gfx_surface *s, int x, int y, char c,
               uint32_t fg, int scale);
void gfx_glyph_bg(struct gfx_surface *s, int x, int y, char c,
                  uint32_t fg, uint32_t bg, int scale);

void gfx_text(struct gfx_surface *s, int x, int y, const char *str,
              uint32_t fg, int scale);
void gfx_text_bg(struct gfx_surface *s, int x, int y, const char *str,
                 uint32_t fg, uint32_t bg, int scale);
void gfx_text_n(struct gfx_surface *s, int x, int y, const char *str,
                size_t n, uint32_t fg, int scale);

void gfx_text_size(const char *str, int scale, int *out_w, int *out_h);
int  gfx_text_fit(const char *str, int scale, int max_w);

/* ---------------- Masks and images -------------------------------------- */

void gfx_mask(struct gfx_surface *s, int x, int y,
              const uint8_t *mask, int mw, int mh,
              uint32_t color, int scale);
void gfx_mask_multi(struct gfx_surface *s, int x, int y,
                    const uint8_t *mask, int mw, int mh,
                    const uint32_t *colors, int ncolors, int scale);

struct gfx_image {
    const uint32_t *pixels;
    int             width, height;
    int             stride;      /* pixels per row; <= 0 means width */
};

void gfx_draw_image(struct gfx_surface *s, int x, int y,
                    const struct gfx_image *img, int scale);

/* Sprite fallback mirrors userspace's mask-or-image drawing model without
 * kernel file loading. Point image at decoded ARGB pixels when available;
 * otherwise drawing falls back to the mask.
 */
struct gfx_sprite {
    struct gfx_image image;       /* pixels != 0 when image art is set */
    const uint8_t   *mask;        /* fallback; mh rows of mw bytes */
    int              mask_w, mask_h;
    uint32_t         colors[3];
    int              ncolors;
};

void gfx_sprite_set_mask(struct gfx_sprite *sp, const uint8_t *mask,
                         int mw, int mh,
                         const uint32_t *colors, int ncolors);
void gfx_sprite_set_image(struct gfx_sprite *sp,
                          const struct gfx_image *image);

int  gfx_sprite_w(const struct gfx_sprite *sp);
int  gfx_sprite_h(const struct gfx_sprite *sp);

void gfx_sprite_draw(struct gfx_surface *s, int x, int y,
                     const struct gfx_sprite *sp, int scale);

#endif
