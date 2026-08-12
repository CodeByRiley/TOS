#include "memory/heap.h"
#include "utilities/string.h"

#define STBTT_malloc(x,u)   kmalloc(x)
#define STBTT_free(x,u)     kfree(x)
#define STBTT_memset        memset
#define STBTT_memcpy        memcpy

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

#define STBTT_ifloor(x) ttf_stb_ifloor((double)(x))
#define STBTT_iceil(x)  ttf_stb_iceil((double)(x))
#define STBTT_fabs(x)   ttf_stb_fabs((double)(x))

#include "ttf.h"
#include "fs/stdio.h"
#include "display/graphics.h"
#include "utilities/log.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

struct ttf_font *g_sys_font = NULL;

int ttf_load(struct ttf_font *font, const char *path) {
    if (!font || !path) return -1;
    memset(font, 0, sizeof(*font));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    font->file_buffer = (u8*)kmalloc(size);
    if (!font->file_buffer) {
        fclose(f);
        return -1;
    }

    if (fread(font->file_buffer, 1, (size_t)size, f) != (size_t)size) {
        kfree(font->file_buffer);
        font->file_buffer = NULL;
        fclose(f);
        return -1;
    }
    fclose(f);

    int offset = stbtt_GetFontOffsetForIndex(font->file_buffer, 0);
    if (offset < 0 || !stbtt_InitFont(&font->info, font->file_buffer, offset)) {
        kfree(font->file_buffer);
        font->file_buffer = NULL;
        return -1;
    }

    return 0;
}

void ttf_free(struct ttf_font *font) {
    if (!font) return;
    if (font->file_buffer) {
        kfree(font->file_buffer);
        font->file_buffer = NULL;
    }
}

void ttf_init_font(void) {
    if (g_sys_font != NULL) return;

    g_sys_font = kmalloc(sizeof(struct ttf_font));
    if (!g_sys_font) {
        log_write("display: failed to allocate TTF font", KERNEL, LOG_ERROR);
        return;
    }

    if (ttf_load(g_sys_font, "/system/fonts/SansDisplayStatic.ttf") != 0) {
        log_write("display: failed to load TTF, using font8x8", KERNEL, LOG_WARN);
        kfree(g_sys_font);
        g_sys_font = NULL;
        return;
    }

    log_write("display: TTF font loaded successfully", KERNEL, LOG_INFO);
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

// Calculates the width of a string if rendered
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

/* Number of leading characters of `s` that fit within max_w pixels. */
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
static void ttf_blit_glyph(struct gfx_surface *s, const u8 *bitmap,
                           int w, int h, int left, int baseline,
                           int xoff, int yoff, u32 color) {
    u8 fg_r = (color >> 16) & 0xFF;
    u8 fg_g = (color >> 8)  & 0xFF;
    u8 fg_b =  color        & 0xFF;

    for (int j = 0; j < h; j++) {
        for (int k = 0; k < w; k++) {
            u8 alpha = bitmap[j * w + k];
            if (alpha == 0) continue;

            int px = left + xoff + k;
            int py = baseline + yoff + j;
            if (px < 0 || px >= s->w || py < 0 || py >= s->h) continue;

            u32 *dest = &s->px[py * s->stride + px];
            u32 bg = *dest;
            u8 bg_r = (bg >> 16) & 0xFF;
            u8 bg_g = (bg >> 8)  & 0xFF;
            u8 bg_b =  bg        & 0xFF;

            u8 new_r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
            u8 new_g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
            u8 new_b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;

            *dest = (0xFFu << 24) | ((u32)new_r << 16) | ((u32)new_g << 8) | new_b;
        }
    }
}

/* Cell width for laying this font out on a fixed grid (the tty). Based on the
 * digit advance, not the widest glyph: digits share one advance in nearly
 * every font and sit close to the typical letter, whereas sizing off 'M'/'W'
 * pads every narrow glyph with the difference — which reads as a space after
 * every character. +1 for inter-character breathing room. */
int ttf_cell_width(struct ttf_font *font, int px_size) {
    if (!font || !font->file_buffer || px_size <= 0) return 0;

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

/* Draw one codepoint centred in a cell_w-wide cell. Glyphs wider than the cell
 * are condensed horizontally rather than clipped — a squeezed 'M' still reads
 * as an 'M', a cropped one doesn't. */
void ttf_draw_glyph_cell(struct gfx_surface *s, struct ttf_font *font,
                         int cell_x, int baseline, int cell_w, int cp,
                         int px_size, u32 color) {
    if (!s || !s->px || !font || !font->file_buffer || px_size <= 0 ||
        cell_w <= 0)
        return;

    float scale = stbtt_ScaleForPixelHeight(&font->info, px_size);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->info, cp, &advance, &lsb);

    int adv = ttf_round(advance * scale);
    float scale_x = scale;

    int fit = cell_w - 1;
    if (fit > 0 && adv > fit) {
        scale_x = scale * (float)fit / (float)adv;
        adv = fit;
    }

    int left = cell_x + (cell_w - adv) / 2;
    if (left < cell_x) left = cell_x;

    int w, h, xoff, yoff;
    u8 *bitmap = stbtt_GetCodepointBitmapSubpixel(&font->info, scale_x, scale,
                                                  0.0f, 0.0f, cp,
                                                  &w, &h, &xoff, &yoff);
    if (!bitmap) return;

    ttf_blit_glyph(s, bitmap, w, h, left, baseline, xoff, yoff, color);
    stbtt_FreeBitmap(bitmap, font->info.userdata);
}

// Draws text directly to a graphics surface
void ttf_draw_text(struct gfx_surface *s, struct ttf_font *font,
                   int x, int baseline, const char *text,
                   int px_size, u32 color) {
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

        // Get the bitmap for this character
        int w, h, xoff, yoff;
        u8 *bitmap = stbtt_GetCodepointBitmapSubpixel(&font->info, scale, scale,
                                                      x_shift, 0.0f, cp,
                                                      &w, &h, &xoff, &yoff);

        // Alpha-blend the 8-bit grayscale bitmap onto the 32-bit framebuffer
        if (bitmap) {
            ttf_blit_glyph(s, bitmap, w, h, draw_x, baseline, xoff, yoff, color);
            // stb_truetype uses STBTT_malloc, so we must free the bitmap
            stbtt_FreeBitmap(bitmap, font->info.userdata);
        }

        pen_x += advance * scale;
    }
}
