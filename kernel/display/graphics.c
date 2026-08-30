/* kernel/display/graphics.c - 2D drawing over a pixel buffer.
 *
 * Mirrors userspace/lib/gfx.c inside the kernel. Every public draw entry
 * point clips before it writes, so callers can pass negative origins,
 * oversized rectangles, or off-surface coordinates without guarding first.
 */
#include <display/graphics.h>
#include <display/fonts/font8x8.h>
#include <utilities/string.h>

/* Rectangles */

struct gfx_rect gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return gfx_rect_make(x0, y0, x1 - x0, y1 - y0);
}

struct gfx_rect gfx_rect_inset(struct gfx_rect r, int n) {
    return gfx_rect_make(r.x + n, r.y + n, r.w - 2 * n, r.h - 2 * n);
}

/* Surfaces */

void gfx_surface_init(struct gfx_surface *s, u32 *px,
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

SINLINE u32 *row_at(const struct gfx_surface *s, int y) {
    return s->px + (usize)y * (usize)s->stride;
}

/* Pixels */

void gfx_pixel(struct gfx_surface *s, int x, int y, u32 color) {
    if (!gfx_rect_contains(s->clip, x, y)) return;
    row_at(s, y)[x] = color & 0x00FFFFFFu;
}

SINLINE void blend_into(u32 *dst, u32 argb) {
    u32 a = argb >> 24;
    if (a == 0) return;
    if (a == 255) { *dst = argb & 0x00FFFFFFu; return; }

    u32 d = *dst;
    u32 inv = 255u - a;
    u32 r = (((argb >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * inv) / 255u;
    u32 g = (((argb >>  8) & 0xFF) * a + ((d >>  8) & 0xFF) * inv) / 255u;
    u32 b = (( argb        & 0xFF) * a + ( d        & 0xFF) * inv) / 255u;
    *dst = (r << 16) | (g << 8) | b;
}

void gfx_blend(struct gfx_surface *s, int x, int y, u32 argb) {
    if (!gfx_rect_contains(s->clip, x, y)) return;
    blend_into(&row_at(s, y)[x], argb);
}

/* Fills */

void gfx_fill(struct gfx_surface *s, struct gfx_rect r, u32 color) {
    struct gfx_rect c = clip_span(s, r, 0, 0);
    if (gfx_rect_empty(c)) return;

    color &= 0x00FFFFFFu;
    for (int y = 0; y < c.h; y++) {
        u32 *p = row_at(s, c.y + y) + c.x;
        for (int x = 0; x < c.w; x++) p[x] = color;
    }
}

void gfx_clear(struct gfx_surface *s, u32 color) {
    gfx_fill(s, gfx_surface_bounds(s), color);
}

void gfx_fill_blend(struct gfx_surface *s, struct gfx_rect r, u32 argb) {
    struct gfx_rect c = clip_span(s, r, 0, 0);
    if (gfx_rect_empty(c)) return;

    for (int y = 0; y < c.h; y++) {
        u32 *p = row_at(s, c.y + y) + c.x;
        for (int x = 0; x < c.w; x++) blend_into(&p[x], argb);
    }
}

void gfx_hline(struct gfx_surface *s, int x, int y, int w, u32 color) {
    gfx_fill(s, gfx_rect_make(x, y, w, 1), color);
}

void gfx_vline(struct gfx_surface *s, int x, int y, int h, u32 color) {
    gfx_fill(s, gfx_rect_make(x, y, 1, h), color);
}

void gfx_frame(struct gfx_surface *s, struct gfx_rect r,
               u32 color, int thickness) {
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
               u32 light, u32 dark, int thickness) {
    if (thickness <= 0 || gfx_rect_empty(r)) return;

    for (int i = 0; i < thickness; i++) {
        gfx_hline(s, r.x + i, r.y + i, r.w - 2 * i, light);
        gfx_vline(s, r.x + i, r.y + i, r.h - 2 * i, light);
        gfx_hline(s, r.x + i, r.y + r.h - 1 - i, r.w - i, dark);
        gfx_vline(s, r.x + r.w - 1 - i, r.y + i, r.h - i, dark);
    }
}

/* Blitting */

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
        const u32 *sp = src->px
            + (usize)(sr.y + skip_y + y) * (usize)src->stride
            + (usize)(sr.x + skip_x);
        u32 *dp = row_at(dst, c.y + y) + c.x;
        memcpy(dp, sp, (usize)c.w * sizeof(u32));
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
        const u32 *sp = src->px
            + (usize)(sr.y + skip_y + y) * (usize)src->stride
            + (usize)(sr.x + skip_x);
        u32 *dp = row_at(dst, c.y + y) + c.x;
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
        const u32 *sp = src->px + (usize)sy * (usize)src->stride;
        u32 *dp = row_at(dst, c.y + y) + c.x;

        for (int x = 0; x < c.w; x++) {
            u32 v = sp[sr.x + (skip_x + x) / scale];
            if (use_alpha) blend_into(&dp[x], v);
            else           dp[x] = v & 0x00FFFFFFu;
        }
    }
}

/* Text */

static const u8 *glyph_rows(char c) {
    unsigned char u = (unsigned char)c;
    if (u < FONT_FIRST || u > FONT_LAST) return 0;
    return font8x8[u - FONT_FIRST];
}

static void glyph_common(struct gfx_surface *s, int x, int y, char c,
                         u32 fg, u32 bg, int have_bg, int scale) {
    if (scale < 1) scale = 1;

    if (have_bg)
        gfx_fill(s, gfx_rect_make(x, y, GFX_GLYPH_W * scale,
                                  GFX_GLYPH_H * scale), bg);

    const u8 *rows = glyph_rows(c);
    if (!rows) return;

    for (int gy = 0; gy < GFX_GLYPH_H; gy++) {
        u8 bits = rows[gy];
        if (!bits) continue;
        for (int gx = 0; gx < GFX_GLYPH_W; gx++) {
            if (!(bits & (1u << gx))) continue;
            gfx_fill(s, gfx_rect_make(x + gx * scale, y + gy * scale,
                                      scale, scale), fg);
        }
    }
}

void gfx_glyph(struct gfx_surface *s, int x, int y, char c,
               u32 fg, int scale) {
    glyph_common(s, x, y, c, fg, 0, 0, scale);
}

void gfx_glyph_bg(struct gfx_surface *s, int x, int y, char c,
                  u32 fg, u32 bg, int scale) {
    glyph_common(s, x, y, c, fg, bg, 1, scale);
}

void gfx_text_n(struct gfx_surface *s, int x, int y, const char *str,
                usize n, u32 fg, int scale) {
    if (!str) return;
    if (scale < 1) scale = 1;
    for (usize i = 0; i < n && str[i]; i++)
        gfx_glyph(s, x + (int)i * GFX_GLYPH_W * scale, y, str[i], fg, scale);
}

void gfx_text(struct gfx_surface *s, int x, int y, const char *str,
              u32 fg, int scale) {
    if (!str) return;
    if (scale < 1) scale = 1;
    for (int i = 0; str[i]; i++)
        gfx_glyph(s, x + i * GFX_GLYPH_W * scale, y, str[i], fg, scale);
}

void gfx_text_bg(struct gfx_surface *s, int x, int y, const char *str,
                 u32 fg, u32 bg, int scale) {
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

/* Masks and images */

void gfx_mask_multi(struct gfx_surface *s, int x, int y,
                    const u8 *mask, int mw, int mh,
                    const u32 *colors, int ncolors, int scale) {
    if (!mask || !colors || ncolors <= 0) return;
    if (scale < 1) scale = 1;

    for (int my = 0; my < mh; my++) {
        for (int mx = 0; mx < mw; mx++) {
            u8 v = mask[(usize)my * (usize)mw + (usize)mx];
            if (v == 0 || v > ncolors) continue;
            gfx_fill(s, gfx_rect_make(x + mx * scale, y + my * scale,
                                      scale, scale), colors[v - 1]);
        }
    }
}

void gfx_mask(struct gfx_surface *s, int x, int y,
              const u8 *mask, int mw, int mh,
              u32 color, int scale) {
    u32 colors[3] = { color, color, color };
    gfx_mask_multi(s, x, y, mask, mw, mh, colors, 3, scale);
}

void gfx_draw_image(struct gfx_surface *s, int x, int y,
                    const struct gfx_image *img, int scale) {
    if (!img || !img->pixels) return;

    struct gfx_surface src;
    gfx_surface_init(&src, (u32 *)img->pixels, img->width, img->height,
                     img->stride > 0 ? img->stride : img->width);
    gfx_blit_scaled(s, x, y, &src, gfx_rect_make(0, 0, 0, 0), scale, 1);
}

/* Sprites */

void gfx_sprite_set_mask(struct gfx_sprite *sp, const u8 *mask,
                         int mw, int mh,
                         const u32 *colors, int ncolors) {
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

void gfx_sprite_set_image(struct gfx_sprite *sp,
                          const struct gfx_image *image) {
    if (!sp) return;
    if (image) sp->image = *image;
    else       sp->image = (struct gfx_image){ 0, 0, 0, 0 };
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
        gfx_draw_image(s, x, y, &sp->image, scale);
    else
        gfx_mask_multi(s, x, y, sp->mask, sp->mask_w, sp->mask_h,
                       sp->colors, sp->ncolors, scale);
}
