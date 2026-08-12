/* userspace/lib/gfx.c — 2D drawing over a pixel buffer.
 *
 * Every entry point clips before it writes, so callers can pass negative
 * origins, oversized rectangles or off-surface coordinates without
 * guarding first. That is deliberate: the alternative is the same three
 * clamp lines copied into every drawing site, which is exactly what the
 * window manager and its clients each grew separately.
 *
 * The clipping all funnels through clip_span, which turns a requested
 * destination rectangle into the part that survives, plus the offset into
 * the source that corresponds to it.
 */
#include "gfx.h"
#include "../include/fonts/font8x8.h"

extern void *memcpy(void *, const void *, size_t);

/* ---------------- Rectangles -------------------------------------------- */

struct gfx_rect gfx_rect_offset(struct gfx_rect r, int dx, int dy) {
    return gfx_rect_make(r.x + dx, r.y + dy, r.w, r.h);
}

struct gfx_rect gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return gfx_rect_make(x0, y0, x1 - x0, y1 - y0);
}

struct gfx_rect gfx_rect_union(struct gfx_rect a, struct gfx_rect b) {
    if (gfx_rect_empty(a)) return b;
    if (gfx_rect_empty(b)) return a;

    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return gfx_rect_make(x0, y0, x1 - x0, y1 - y0);
}

struct gfx_rect gfx_rect_inset(struct gfx_rect r, int n) {
    return gfx_rect_make(r.x + n, r.y + n, r.w - 2 * n, r.h - 2 * n);
}

/* ---------------- Surfaces ---------------------------------------------- */

void gfx_surface_init(struct gfx_surface *s, uint32_t *px,
                      int w, int h, int stride) {
    s->px     = px;
    s->w      = w;
    s->h      = h;
    s->stride = stride > 0 ? stride : w;
    s->clip   = gfx_rect_make(0, 0, w, h);
}

void gfx_clip_set(struct gfx_surface *s, struct gfx_rect r) {
    s->clip = gfx_rect_intersect(r, gfx_surface_bounds(s));
}

struct gfx_rect gfx_clip_push(struct gfx_surface *s, struct gfx_rect r) {
    struct gfx_rect prev = s->clip;
    s->clip = gfx_rect_intersect(prev, r);
    return prev;
}

void gfx_clip_reset(struct gfx_surface *s) {
    s->clip = gfx_surface_bounds(s);
}

/* Clip `want` against the surface, reporting how far the top-left corner
 * moved so a source can be advanced by the same amount. Returns an empty
 * rect when nothing survives. */
static struct gfx_rect clip_span(const struct gfx_surface *s,
                                 struct gfx_rect want,
                                 int *skip_x, int *skip_y) {
    struct gfx_rect out = gfx_rect_intersect(want, s->clip);
    if (skip_x) *skip_x = out.x - want.x;
    if (skip_y) *skip_y = out.y - want.y;
    if (out.w < 0) out.w = 0;
    if (out.h < 0) out.h = 0;
    return out;
}

static inline uint32_t *row_at(const struct gfx_surface *s, int y) {
    return s->px + (size_t)y * (size_t)s->stride;
}

/* ---------------- Pixels ------------------------------------------------- */

void gfx_pixel(struct gfx_surface *s, int x, int y, uint32_t color) {
    if (!gfx_rect_contains(s->clip, x, y)) return;
    row_at(s, y)[x] = color & 0x00FFFFFFu;
}

/* Straight (non-premultiplied) source-over. Fully opaque and fully
 * transparent skip the arithmetic; they are the overwhelming majority of
 * pixels in the sprites this draws. */
static inline void blend_into(uint32_t *dst, uint32_t argb) {
    uint32_t a = argb >> 24;
    if (a == 0) return;
    if (a == 255) { *dst = argb & 0x00FFFFFFu; return; }

    uint32_t d = *dst;
    uint32_t inv = 255u - a;
    uint32_t r = (((argb >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * inv) / 255u;
    uint32_t g = (((argb >>  8) & 0xFF) * a + ((d >>  8) & 0xFF) * inv) / 255u;
    uint32_t b = (( argb        & 0xFF) * a + ( d        & 0xFF) * inv) / 255u;
    *dst = (r << 16) | (g << 8) | b;
}

void gfx_blend(struct gfx_surface *s, int x, int y, uint32_t argb) {
    if (!gfx_rect_contains(s->clip, x, y)) return;
    blend_into(&row_at(s, y)[x], argb);
}

/* ---------------- Fills --------------------------------------------------- */

void gfx_fill(struct gfx_surface *s, struct gfx_rect r, uint32_t color) {
    struct gfx_rect c = clip_span(s, r, 0, 0);
    if (gfx_rect_empty(c)) return;

    color &= 0x00FFFFFFu;
    for (int y = 0; y < c.h; y++) {
        uint32_t *p = row_at(s, c.y + y) + c.x;
        for (int x = 0; x < c.w; x++) p[x] = color;
    }
}

void gfx_clear(struct gfx_surface *s, uint32_t color) {
    gfx_fill(s, gfx_surface_bounds(s), color);
}

void gfx_fill_blend(struct gfx_surface *s, struct gfx_rect r, uint32_t argb) {
    struct gfx_rect c = clip_span(s, r, 0, 0);
    if (gfx_rect_empty(c)) return;

    for (int y = 0; y < c.h; y++) {
        uint32_t *p = row_at(s, c.y + y) + c.x;
        for (int x = 0; x < c.w; x++) blend_into(&p[x], argb);
    }
}

void gfx_hline(struct gfx_surface *s, int x, int y, int w, uint32_t color) {
    gfx_fill(s, gfx_rect_make(x, y, w, 1), color);
}

void gfx_vline(struct gfx_surface *s, int x, int y, int h, uint32_t color) {
    gfx_fill(s, gfx_rect_make(x, y, 1, h), color);
}

void gfx_frame(struct gfx_surface *s, struct gfx_rect r,
               uint32_t color, int thickness) {
    if (thickness <= 0 || gfx_rect_empty(r)) return;
    if (thickness > r.w) thickness = r.w;
    if (thickness > r.h) thickness = r.h;

    gfx_fill(s, gfx_rect_make(r.x, r.y, r.w, thickness), color);
    gfx_fill(s, gfx_rect_make(r.x, r.y + r.h - thickness,
                              r.w, thickness), color);
    gfx_fill(s, gfx_rect_make(r.x, r.y, thickness, r.h), color);
    gfx_fill(s, gfx_rect_make(r.x + r.w - thickness, r.y,
                              thickness, r.h), color);
}

void gfx_bevel(struct gfx_surface *s, struct gfx_rect r,
               uint32_t light, uint32_t dark, int thickness) {
    if (thickness <= 0 || gfx_rect_empty(r)) return;

    for (int i = 0; i < thickness; i++) {
        gfx_hline(s, r.x + i, r.y + i, r.w - 2 * i, light);
        gfx_vline(s, r.x + i, r.y + i, r.h - 2 * i, light);
        gfx_hline(s, r.x + i, r.y + r.h - 1 - i, r.w - i, dark);
        gfx_vline(s, r.x + r.w - 1 - i, r.y + i, r.h - i, dark);
    }
}

/* ---------------- Blitting ------------------------------------------------ */

/* An empty source rect means "all of it" — the common case, and it keeps
 * callers from having to spell out the full bounds. */
static struct gfx_rect src_or_all(const struct gfx_surface *src,
                                  struct gfx_rect r) {
    if (gfx_rect_empty(r))
        return gfx_rect_make(0, 0, src->w, src->h);
    return gfx_rect_intersect(r, gfx_rect_make(0, 0, src->w, src->h));
}

void gfx_blit(struct gfx_surface *dst, int dx, int dy,
              const struct gfx_surface *src, struct gfx_rect src_rect) {
    struct gfx_rect sr = src_or_all(src, src_rect);
    if (gfx_rect_empty(sr)) return;

    int skip_x, skip_y;
    struct gfx_rect c = clip_span(dst, gfx_rect_make(dx, dy, sr.w, sr.h),
                                  &skip_x, &skip_y);
    if (gfx_rect_empty(c)) return;

    for (int y = 0; y < c.h; y++) {
        const uint32_t *sp = src->px
            + (size_t)(sr.y + skip_y + y) * (size_t)src->stride
            + (size_t)(sr.x + skip_x);
        uint32_t *dp = row_at(dst, c.y + y) + c.x;
        memcpy(dp, sp, (size_t)c.w * sizeof(uint32_t));
    }
}

void gfx_blit_alpha(struct gfx_surface *dst, int dx, int dy,
                    const struct gfx_surface *src, struct gfx_rect src_rect) {
    struct gfx_rect sr = src_or_all(src, src_rect);
    if (gfx_rect_empty(sr)) return;

    int skip_x, skip_y;
    struct gfx_rect c = clip_span(dst, gfx_rect_make(dx, dy, sr.w, sr.h),
                                  &skip_x, &skip_y);
    if (gfx_rect_empty(c)) return;

    for (int y = 0; y < c.h; y++) {
        const uint32_t *sp = src->px
            + (size_t)(sr.y + skip_y + y) * (size_t)src->stride
            + (size_t)(sr.x + skip_x);
        uint32_t *dp = row_at(dst, c.y + y) + c.x;
        for (int x = 0; x < c.w; x++) blend_into(&dp[x], sp[x]);
    }
}

void gfx_blit_scaled(struct gfx_surface *dst, int dx, int dy,
                     const struct gfx_surface *src, struct gfx_rect src_rect,
                     int scale, int use_alpha) {
    if (scale < 1) scale = 1;

    struct gfx_rect sr = src_or_all(src, src_rect);
    if (gfx_rect_empty(sr)) return;

    int skip_x, skip_y;
    struct gfx_rect c = clip_span(dst,
        gfx_rect_make(dx, dy, sr.w * scale, sr.h * scale), &skip_x, &skip_y);
    if (gfx_rect_empty(c)) return;

    for (int y = 0; y < c.h; y++) {
        int sy = sr.y + (skip_y + y) / scale;
        const uint32_t *sp = src->px + (size_t)sy * (size_t)src->stride;
        uint32_t *dp = row_at(dst, c.y + y) + c.x;

        for (int x = 0; x < c.w; x++) {
            uint32_t v = sp[sr.x + (skip_x + x) / scale];
            if (use_alpha) blend_into(&dp[x], v);
            else           dp[x] = v & 0x00FFFFFFu;
        }
    }
}

/* ---------------- Text ----------------------------------------------------- */

/* Rows of the glyph, or NULL for anything outside the font. Bit 0 of each
 * row byte is the leftmost pixel. */
static const uint8_t *glyph_rows(char c) {
    unsigned char u = (unsigned char)c;
    if (u < FONT_FIRST || u > FONT_LAST) return 0;
    return font8x8[u - FONT_FIRST];
}

/* One glyph. `bg` is drawn first when `have_bg`, which is what makes text
 * over a busy background legible without a separate fill call. */
static void glyph_common(struct gfx_surface *s, int x, int y, char c,
                         uint32_t fg, uint32_t bg, int have_bg, int scale) {
    if (scale < 1) scale = 1;

    if (have_bg)
        gfx_fill(s, gfx_rect_make(x, y, GFX_GLYPH_W * scale,
                                  GFX_GLYPH_H * scale), bg);

    const uint8_t *rows = glyph_rows(c);
    if (!rows) return;

    for (int gy = 0; gy < GFX_GLYPH_H; gy++) {
        uint8_t bits = rows[gy];
        if (!bits) continue;
        for (int gx = 0; gx < GFX_GLYPH_W; gx++) {
            if (!(bits & (1u << gx))) continue;
            gfx_fill(s, gfx_rect_make(x + gx * scale, y + gy * scale,
                                      scale, scale), fg);
        }
    }
}

void gfx_glyph(struct gfx_surface *s, int x, int y, char c,
               uint32_t fg, int scale) {
    glyph_common(s, x, y, c, fg, 0, 0, scale);
}

void gfx_glyph_bg(struct gfx_surface *s, int x, int y, char c,
                  uint32_t fg, uint32_t bg, int scale) {
    glyph_common(s, x, y, c, fg, bg, 1, scale);
}

void gfx_text_n(struct gfx_surface *s, int x, int y, const char *str,
                size_t n, uint32_t fg, int scale) {
    if (!str) return;
    if (scale < 1) scale = 1;
    for (size_t i = 0; i < n && str[i]; i++)
        gfx_glyph(s, x + (int)i * GFX_GLYPH_W * scale, y, str[i], fg, scale);
}

void gfx_text(struct gfx_surface *s, int x, int y, const char *str,
              uint32_t fg, int scale) {
    if (!str) return;
    if (scale < 1) scale = 1;
    for (int i = 0; str[i]; i++)
        gfx_glyph(s, x + i * GFX_GLYPH_W * scale, y, str[i], fg, scale);
}

void gfx_text_bg(struct gfx_surface *s, int x, int y, const char *str,
                 uint32_t fg, uint32_t bg, int scale) {
    if (!str) return;
    if (scale < 1) scale = 1;
    for (int i = 0; str[i]; i++)
        gfx_glyph_bg(s, x + i * GFX_GLYPH_W * scale, y, str[i],
                     fg, bg, scale);
}

void gfx_text_size(const char *str, int scale, int *out_w, int *out_h) {
    if (scale < 1) scale = 1;
    int n = 0;
    if (str) while (str[n]) n++;
    if (out_w) *out_w = n * GFX_GLYPH_W * scale;
    if (out_h) *out_h = str ? GFX_GLYPH_H * scale : 0;
}

int gfx_text_fit(const char *str, int scale, int max_w) {
    if (!str || max_w <= 0) return 0;
    if (scale < 1) scale = 1;
    int cell = GFX_GLYPH_W * scale;
    int n = 0;
    while (str[n] && (n + 1) * cell <= max_w) n++;
    return n;
}

static int text_box_common(struct gfx_surface *s, struct gfx_rect box,
                           const char *str, uint32_t fg, uint32_t bg,
                           int have_bg, int scale, int pad,
                           enum gfx_text_align align) {
    if (!s || !str || gfx_rect_empty(box)) return 0;
    if (scale < 1) scale = 1;
    if (pad < 0) pad = 0;

    if (have_bg)
        gfx_fill(s, box, bg);

    int avail = box.w - 2 * pad;
    if (avail <= 0) return 0;

    int n = gfx_text_fit(str, scale, avail);
    if (n <= 0) return 0;

    int tw = n * GFX_GLYPH_W * scale;
    int th = GFX_GLYPH_H * scale;
    int x = box.x + pad;
    if (align == GFX_TEXT_CENTER)
        x = box.x + (box.w - tw) / 2;
    else if (align == GFX_TEXT_RIGHT)
        x = box.x + box.w - pad - tw;

    int y = box.y + (box.h - th) / 2;

    struct gfx_rect prev = gfx_clip_push(s, box);
    gfx_text_n(s, x, y, str, (size_t)n, fg, scale);
    gfx_clip_set(s, prev);
    return n;
}

int gfx_text_box(struct gfx_surface *s, struct gfx_rect box,
                 const char *str, uint32_t fg, int scale,
                 int pad, enum gfx_text_align align) {
    return text_box_common(s, box, str, fg, 0, 0, scale, pad, align);
}

int gfx_text_box_bg(struct gfx_surface *s, struct gfx_rect box,
                    const char *str, uint32_t fg, uint32_t bg, int scale,
                    int pad, enum gfx_text_align align) {
    return text_box_common(s, box, str, fg, bg, 1, scale, pad, align);
}

/* ---------------- Masks and images ----------------------------------------- */

void gfx_mask_multi(struct gfx_surface *s, int x, int y,
                    const uint8_t *mask, int mw, int mh,
                    const uint32_t *colors, int ncolors, int scale) {
    if (!mask || !colors || ncolors <= 0) return;
    if (scale < 1) scale = 1;

    for (int my = 0; my < mh; my++) {
        for (int mx = 0; mx < mw; mx++) {
            uint8_t v = mask[(size_t)my * mw + mx];
            if (v == 0 || v > ncolors) continue;
            gfx_fill(s, gfx_rect_make(x + mx * scale, y + my * scale,
                                      scale, scale), colors[v - 1]);
        }
    }
}

void gfx_mask(struct gfx_surface *s, int x, int y,
              const uint8_t *mask, int mw, int mh,
              uint32_t color, int scale) {
    /* Every non-zero value maps to the same colour. Three entries covers
     * the 0..3 range the built-in masks use. */
    uint32_t colors[3] = { color, color, color };
    gfx_mask_multi(s, x, y, mask, mw, mh, colors, 3, scale);
}

void gfx_draw_bmp(struct gfx_surface *s, int x, int y,
                  const struct bmp_image *img, int scale) {
    if (!img || !img->pixels) return;

    struct gfx_surface src;
    gfx_surface_init(&src, img->pixels, img->width, img->height, img->width);
    gfx_blit_scaled(s, x, y, &src, gfx_rect_make(0, 0, 0, 0), scale, 1);
}

/* ---------------- Sprites ---------------------------------------------------- */

void gfx_sprite_set_mask(struct gfx_sprite *sp, const uint8_t *mask,
                         int mw, int mh,
                         const uint32_t *colors, int ncolors) {
    if (!sp) return;
    sp->mask   = mask;
    sp->mask_w = mw;
    sp->mask_h = mh;
    sp->ncolors = 0;
    for (int i = 0; i < ncolors && i < 3; i++) {
        sp->colors[i] = colors[i];
        sp->ncolors++;
    }
}

int gfx_sprite_load(struct gfx_sprite *sp, const char *path) {
    if (!sp) return -1;

    struct bmp_image img;
    if (bmp_load(path, &img) != 0) return -1;   /* fallback survives */

    bmp_free(&sp->image);
    sp->image = img;
    return 0;
}

void gfx_sprite_free(struct gfx_sprite *sp) {
    if (!sp) return;
    bmp_free(&sp->image);
}

int gfx_sprite_w(const struct gfx_sprite *sp) {
    if (!sp) return 0;
    return sp->image.pixels ? sp->image.width : sp->mask_w;
}

int gfx_sprite_h(const struct gfx_sprite *sp) {
    if (!sp) return 0;
    return sp->image.pixels ? sp->image.height : sp->mask_h;
}

void gfx_sprite_draw(struct gfx_surface *s, int x, int y,
                     const struct gfx_sprite *sp, int scale) {
    if (!sp) return;
    if (sp->image.pixels)
        gfx_draw_bmp(s, x, y, &sp->image, scale);
    else
        gfx_mask_multi(s, x, y, sp->mask, sp->mask_w, sp->mask_h,
                       sp->colors, sp->ncolors, scale);
}
