#include "console.h"
#include "syscall.h"
#include "../include/font8x8.h"
#include <stdint.h>
#include <stddef.h>

static struct fb_info  fbi;
static uint32_t       *fb;
static uint32_t        cols, rows;
static uint32_t        cx, cy;
static uint32_t        fg = 0x00FFFFFF;        /* white */
static uint32_t        bg = 0x00000000;        /* black */
static int             initialised = 0;

static void draw_glyph(uint32_t gx, uint32_t gy, char c) {
    uint32_t px = gx * FONT_GLYPH_W;
    uint32_t py = gy * FONT_GLYPH_H;
    if (px + FONT_GLYPH_W > fbi.width) return;
    if (py + FONT_GLYPH_H > fbi.height) return;

    const uint8_t *glyph;
    if (c < FONT_FIRST || c > FONT_LAST) {
        /* unknown char: draw blank */
        static const uint8_t blank[8] = {0,0,0,0,0,0,0,0};
        glyph = blank;
    } else {
        glyph = font8x8[(int)c - FONT_FIRST];
    }

    for (uint32_t row = 0; row < FONT_GLYPH_H; row++) {
        uint8_t   bits = glyph[row];
        uint32_t *line = (uint32_t*)((uint8_t*)fb + (py + row) * fbi.pitch + px * 4);
        for (uint32_t col = 0; col < FONT_GLYPH_W; col++) {
            line[col] = (bits & (1 << col)) ? fg : bg;
        }
    }
}

static void scroll_one(void) {
    /* shift screen up by FONT_GLYPH_H pixels */
    uint8_t *base = (uint8_t*)fb;
    uint32_t row_bytes = fbi.pitch;
    uint32_t shift_pixels = FONT_GLYPH_H;
    uint32_t copy_rows    = fbi.height - shift_pixels;

    for (uint32_t y = 0; y < copy_rows; y++) {
        uint32_t *dst = (uint32_t*)(base + y * row_bytes);
        uint32_t *src = (uint32_t*)(base + (y + shift_pixels) * row_bytes);
        for (uint32_t x = 0; x < fbi.width; x++) dst[x] = src[x];
    }
    /* clear bottom row */
    for (uint32_t y = copy_rows; y < fbi.height; y++) {
        uint32_t *line = (uint32_t*)(base + y * row_bytes);
        for (uint32_t x = 0; x < fbi.width; x++) line[x] = bg;
    }
}

static void newline(void) {
    cx = 0;
    cy++;
    if (cy >= rows) {
        scroll_one();
        cy = rows - 1;
    }
}

void console_init(void) {
    if (initialised) return;
    fb_info(&fbi);
    fb   = (uint32_t*)fb_map();
    cols = fbi.width  / FONT_GLYPH_W;
    rows = fbi.height / FONT_GLYPH_H;
    cx = cy = 0;
    initialised = 1;
    console_clear();
}

void console_clear(void) {
    uint8_t *base = (uint8_t*)fb;
    for (uint32_t y = 0; y < fbi.height; y++) {
        uint32_t *line = (uint32_t*)(base + y * fbi.pitch);
        for (uint32_t x = 0; x < fbi.width; x++) line[x] = bg;
    }
    cx = cy = 0;
}

void console_set_color(uint32_t new_fg, uint32_t new_bg) {
    fg = new_fg;
    bg = new_bg;
}

void console_putc(char c) {
    if (!initialised) console_init();

    if (c == '\n') { newline(); return; }
    if (c == '\r') { cx = 0; return; }
    if (c == '\b') {
        if (cx > 0) {
            cx--;
            draw_glyph(cx, cy, ' ');
        }
        return;
    }
    if (c == '\t') {
        do { console_putc(' '); } while (cx % 8);
        return;
    }

    if (cx >= cols) newline();
    draw_glyph(cx, cy, c);
    cx++;
}

void console_puts(const char *s) {
    while (*s) console_putc(*s++);
}

void console_write(const char *buf, size_t len) {
    while (len--) console_putc(*buf++);
}
