/* userspace/lib/ttf.c - small stb_truetype wrapper for TOS userspace.
 *
 * This is intentionally self-contained: stb_truetype is given our allocator,
 * string routines, and a tiny private math subset so userspace programs can
 * link TTF support without pulling in a host C runtime or the DOOM-only math
 * object.
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"

static int ttf_stb_ifloor(double x) {
    int i = (int)x;
    return (double)i > x ? i - 1 : i;
}

static int ttf_stb_iceil(double x) {
    int i = (int)x;
    return (double)i < x ? i + 1 : i;
}

static double ttf_stb_fabs(double x) {
    return x < 0.0 ? -x : x;
}

static double ttf_stb_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x >= 1.0 ? x : 1.0;
    for (int i = 0; i < 32; i++)
        g = 0.5 * (g + x / g);
    return g;
}

static double ttf_stb_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double qd = x / y;
    long long q = (long long)qd;
    return x - (double)q * y;
}

static double ttf_stb_cbrt(double x) {
    if (x == 0.0) return 0.0;
    double sign = x < 0.0 ? -1.0 : 1.0;
    double ax = ttf_stb_fabs(x);
    double g = ax >= 1.0 ? ax : 1.0;
    for (int i = 0; i < 32; i++)
        g = (2.0 * g + ax / (g * g)) / 3.0;
    return sign * g;
}

static double ttf_stb_pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (ttf_stb_fabs(exp - (1.0 / 3.0)) < 0.000001)
        return ttf_stb_cbrt(base);

    long long n = (long long)exp;
    if ((double)n != exp) return 0.0;
    if (n < 0) {
        if (base == 0.0) return 0.0;
        return 1.0 / ttf_stb_pow(base, (double)-n);
    }

    double out = 1.0;
    while (n) {
        if (n & 1) out *= base;
        base *= base;
        n >>= 1;
    }
    return out;
}

#define TTF_PI     3.14159265358979323846
#define TTF_PI_2   1.57079632679489661923
#define TTF_TWO_PI 6.28318530717958647692

static double ttf_reduce_angle(double x) {
    x = ttf_stb_fmod(x, TTF_TWO_PI);
    if (x > TTF_PI) x -= TTF_TWO_PI;
    if (x < -TTF_PI) x += TTF_TWO_PI;
    return x;
}

static double ttf_stb_cos(double x) {
    x = ttf_reduce_angle(x);
    double x2 = x * x;
    return 1.0
        - x2 / 2.0
        + (x2 * x2) / 24.0
        - (x2 * x2 * x2) / 720.0
        + (x2 * x2 * x2 * x2) / 40320.0;
}

static double ttf_stb_atan_unit(double x) {
    double sign = x < 0.0 ? -1.0 : 1.0;
    x = ttf_stb_fabs(x);
    if (x > 1.0)
        return sign * (TTF_PI_2 - ttf_stb_atan_unit(1.0 / x));

    double x2 = x * x;
    double term = x;
    double sum = x;
    int add = 0;
    for (int n = 3; n <= 31; n += 2) {
        term *= x2;
        if (add) sum += term / (double)n;
        else     sum -= term / (double)n;
        add = !add;
    }
    return sign * sum;
}

static double ttf_stb_atan2(double y, double x) {
    if (x > 0.0) return ttf_stb_atan_unit(y / x);
    if (x < 0.0 && y >= 0.0) return ttf_stb_atan_unit(y / x) + TTF_PI;
    if (x < 0.0 && y < 0.0) return ttf_stb_atan_unit(y / x) - TTF_PI;
    if (y > 0.0) return TTF_PI_2;
    if (y < 0.0) return -TTF_PI_2;
    return 0.0;
}

static double ttf_stb_asin(double x) {
    if (x > 1.0) x = 1.0;
    if (x < -1.0) x = -1.0;
    return ttf_stb_atan2(x, ttf_stb_sqrt(1.0 - x * x));
}

static double ttf_stb_acos(double x) {
    return TTF_PI_2 - ttf_stb_asin(x);
}

#define STBTT_malloc(x,u)   ((void)(u), malloc(x))
#define STBTT_free(x,u)     ((void)(u), free(x))
#define STBTT_memset        memset
#define STBTT_memcpy        memcpy
#define STBTT_strlen        strlen
#define STBTT_assert(x)     ((void)0)
#define STBTT_ifloor(x)     ttf_stb_ifloor((double)(x))
#define STBTT_iceil(x)      ttf_stb_iceil((double)(x))
#define STBTT_fabs(x)       ttf_stb_fabs((double)(x))
#define STBTT_sqrt(x)       ttf_stb_sqrt((double)(x))
#define STBTT_pow(x,y)      ttf_stb_pow((double)(x), (double)(y))
#define STBTT_fmod(x,y)     ttf_stb_fmod((double)(x), (double)(y))
#define STBTT_cos(x)        ttf_stb_cos((double)(x))
#define STBTT_acos(x)       ttf_stb_acos((double)(x))

#include "ttf.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../include/fonts/stb_truetype.h"

struct ttf_font *g_sys_font = NULL;

int ttf_load(struct ttf_font *font, const char *path) {
    if (!font || !path) return -1;
    memset(font, 0, sizeof(*font));

    FILE *f = fopen(path, "rb");
    if (!f) return -2;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -3;
    }

    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -4;
    }

    font->file_buffer = (uint8_t *)malloc((size_t)size);
    if (!font->file_buffer) {
        fclose(f);
        return -5;
    }

    if (fread(font->file_buffer, 1, (size_t)size, f) != (size_t)size) {
        free(font->file_buffer);
        font->file_buffer = NULL;
        fclose(f);
        return -6;
    }
    fclose(f);

    int offset = stbtt_GetFontOffsetForIndex(font->file_buffer, 0);
    if (offset < 0 || !stbtt_InitFont(&font->info, font->file_buffer, offset)) {
        free(font->file_buffer);
        font->file_buffer = NULL;
        return -7;
    }

    return 0;
}

void ttf_free(struct ttf_font *font) {
    if (!font) return;
    if (font->file_buffer) {
        free(font->file_buffer);
        font->file_buffer = NULL;
    }
}

void ttf_init_font(void) {
    if (g_sys_font != NULL) return;

    g_sys_font = malloc(sizeof(struct ttf_font));
    if (!g_sys_font) return;

    if (ttf_load(g_sys_font, "/system/fonts/sansdisplaystatic.ttf") != 0) {
        free(g_sys_font);
        g_sys_font = NULL;
    }
}

/* Pen positions accumulate in float and only round at the end. Rounding each
 * glyph's advance to an int instead drops up to 0.5px per character, which
 * compounds into visibly wrong spacing a dozen characters into a string. */
static int ttf_round(float v) {
    return (int)(v + (v < 0.0f ? -0.5f : 0.5f));
}

int ttf_char_advance(struct ttf_font *font, int cp, int px_size) {
    if (!font || !font->file_buffer || px_size <= 0) return 0;

    float scale = stbtt_ScaleForPixelHeight(&font->info, px_size);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->info, cp, &advance, &lsb);
    return ttf_round(advance * scale);
}

void ttf_vmetrics(struct ttf_font *font, int px_size,
                  int *ascent, int *descent, int *line_gap) {
    int asc = 0, desc = 0, gap = 0;

    if (font && font->file_buffer && px_size > 0) {
        float scale = stbtt_ScaleForPixelHeight(&font->info, px_size);
        stbtt_GetFontVMetrics(&font->info, &asc, &desc, &gap);
        asc  = ttf_round(asc * scale);
        desc = ttf_round(desc * scale);
        gap  = ttf_round(gap * scale);
    }

    if (ascent)   *ascent   = asc;
    if (descent)  *descent  = desc;
    if (line_gap) *line_gap = gap;
}

int ttf_text_width(struct ttf_font *font, const char *s, int px_size) {
    if (!font || !font->file_buffer || !s || px_size <= 0) return 0;

    float scale = stbtt_ScaleForPixelHeight(&font->info, px_size);
    float x = 0.0f;
    int advance, lsb;

    for (int i = 0; s[i]; i++) {
        int cp = (unsigned char)s[i];

        if (i > 0) {
            int prev = (unsigned char)s[i - 1];
            int kern = stbtt_GetCodepointKernAdvance(&font->info, prev, cp);
            x += kern * scale;
        }

        stbtt_GetCodepointHMetrics(&font->info, cp, &advance, &lsb);
        x += advance * scale;
    }
    return ttf_round(x);
}

int ttf_text_fit(struct ttf_font *font, const char *s, int px_size, int max_w) {
    if (!font || !font->file_buffer || !s || px_size <= 0 || max_w <= 0)
        return 0;

    float scale = stbtt_ScaleForPixelHeight(&font->info, px_size);
    float x = 0.0f;
    int advance, lsb;

    for (int i = 0; s[i]; i++) {
        int cp = (unsigned char)s[i];
        if (i > 0) {
            int prev = (unsigned char)s[i - 1];
            int kern = stbtt_GetCodepointKernAdvance(&font->info, prev, cp);
            x += kern * scale;
        }
        stbtt_GetCodepointHMetrics(&font->info, cp, &advance, &lsb);
        float next = x + advance * scale;
        if (ttf_round(next) > max_w) return i;
        x = next;
    }
    return (int)strlen(s);
}

/* Alpha-blend one rasterized glyph. `left`/`baseline` are the glyph origin;
 * xoff/yoff come from stb and are relative to it. */
static void ttf_blit_glyph(struct gfx_surface *s, const uint8_t *bitmap,
                           int w, int h, int left, int baseline,
                           int xoff, int yoff, uint32_t color) {
    uint8_t fg_r = (color >> 16) & 0xFF;
    uint8_t fg_g = (color >> 8)  & 0xFF;
    uint8_t fg_b =  color        & 0xFF;

    for (int j = 0; j < h; j++) {
        for (int k = 0; k < w; k++) {
            uint8_t alpha = bitmap[j * w + k];
            if (alpha == 0)
                continue;

            int px = left + xoff + k;
            int py = baseline + yoff + j;
            if (!gfx_rect_contains(s->clip, px, py))
                continue;

            uint32_t *dest = &s->px[py * s->stride + px];
            uint32_t bg = *dest;
            uint8_t bg_r = (bg >> 16) & 0xFF;
            uint8_t bg_g = (bg >> 8)  & 0xFF;
            uint8_t bg_b =  bg        & 0xFF;

            uint8_t new_r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
            uint8_t new_g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
            uint8_t new_b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;

            *dest = ((uint32_t)new_r << 16)
                  | ((uint32_t)new_g << 8)
                  | (uint32_t)new_b;
        }
    }
}

void ttf_draw_text(struct gfx_surface *s, struct ttf_font *font,
                   int x, int baseline, const char *text,
                   int px_size, uint32_t color) {
    if (!s || !s->px || !font || !font->file_buffer || !text || px_size <= 0)
        return;

    float scale = stbtt_ScaleForPixelHeight(&font->info, px_size);

    /* Fractional pen. Each glyph is rasterized with the sub-pixel remainder
     * baked in, so a 2.4px-advance glyph lands where it belongs instead of
     * snapping to a whole pixel and dragging the rest of the line with it. */
    float pen_x = (float)x;

    for (int i = 0; text[i]; i++) {
        int cp = (unsigned char)text[i];

        if (i > 0) {
            int prev = (unsigned char)text[i - 1];
            int kern = stbtt_GetCodepointKernAdvance(&font->info, prev, cp);
            pen_x += kern * scale;
        }

        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font->info, cp, &advance, &lsb);

        int draw_x = ttf_stb_ifloor((double)pen_x);
        float x_shift = pen_x - (float)draw_x;

        int w, h, xoff, yoff;
        uint8_t *bitmap = stbtt_GetCodepointBitmapSubpixel(
            &font->info, scale, scale, x_shift, 0.0f, cp,
            &w, &h, &xoff, &yoff);

        if (bitmap) {
            ttf_blit_glyph(s, bitmap, w, h, draw_x, baseline, xoff, yoff,
                           color);
            stbtt_FreeBitmap(bitmap, font->info.userdata);
        }

        pen_x += advance * scale;
    }
}

int ttf_cell_width(struct ttf_font *font, int px_size) {
    if (!font || !font->file_buffer || px_size <= 0) return 0;

    /* Digits are the natural monospace reference: in nearly every font they
     * share one advance, and that advance sits close to the typical letter.
     * Sizing cells off the widest glyph instead ('M'/'W'/'m') pads every
     * narrow glyph with the difference, which reads as a space after every
     * character. +1 for inter-character breathing room. */
    int cell = 0;
    for (int c = '0'; c <= '9'; c++) {
        int a = ttf_char_advance(font, c, px_size);
        if (a > cell) cell = a;
    }

    if (cell <= 0) {
        for (int c = 32; c <= 126; c++) {
            int a = ttf_char_advance(font, c, px_size);
            if (a > cell) cell = a;
        }
    }
    return cell > 0 ? cell + 1 : 0;
}

void ttf_draw_glyph_cell(struct gfx_surface *s, struct ttf_font *font,
                         int cell_x, int baseline, int cell_w, int cp,
                         int px_size, uint32_t color) {
    if (!s || !s->px || !font || !font->file_buffer || px_size <= 0 ||
        cell_w <= 0)
        return;

    float scale = stbtt_ScaleForPixelHeight(&font->info, px_size);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->info, cp, &advance, &lsb);

    int adv = ttf_round(advance * scale);
    float scale_x = scale;

    /* Glyphs wider than the cell get condensed horizontally rather than
     * clipped — a squeezed 'M' still reads as an 'M', a cropped one doesn't.
     * Narrow glyphs keep their shape and are simply centred. */
    int fit = cell_w - 1;
    if (fit > 0 && adv > fit) {
        scale_x = scale * (float)fit / (float)adv;
        adv = fit;
    }

    int left = cell_x + (cell_w - adv) / 2;
    if (left < cell_x)
        left = cell_x;

    int w, h, xoff, yoff;
    uint8_t *bitmap = stbtt_GetCodepointBitmapSubpixel(
        &font->info, scale_x, scale, 0.0f, 0.0f, cp, &w, &h, &xoff, &yoff);
    if (!bitmap)
        return;

    ttf_blit_glyph(s, bitmap, w, h, left, baseline, xoff, yoff, color);
    stbtt_FreeBitmap(bitmap, font->info.userdata);
}
