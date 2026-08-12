/* kernel/display/fonts/ttf.h — minimal truetype font.
 *
 *
 * Implementation: kernel/display/fonts/ttf.c.
 */
#ifndef TTF_H
#define TTF_H

#include "utilities/types.h"
#include "display/graphics.h"
#include "stb_truetype.h"


struct ttf_font {
    u8 *file_buffer;          // Raw TTF file data loaded into memory
    stbtt_fontinfo info;      // stb_truetype's internal state
    float scale;              // Cached scale factor for px_size
};

extern struct ttf_font *g_sys_font;

int  ttf_load(struct ttf_font *font, const char *path);
void ttf_free(struct ttf_font *font);

void ttf_init_font(void);

int  ttf_text_width(struct ttf_font *font, const char *s, int px_size);
int  ttf_text_fit(struct ttf_font *font, const char *s, int px_size, int max_w);

/* Pixel advance of a single codepoint. Callers laying text out on a fixed
 * cell grid need this to centre a proportional glyph in its cell. */
int  ttf_char_advance(struct ttf_font *font, int cp, int px_size);

/* Scaled vertical metrics; descent is negative. Any out param may be NULL. */
void ttf_vmetrics(struct ttf_font *font, int px_size,
                  int *ascent, int *descent, int *line_gap);

void ttf_draw_text(struct gfx_surface *s, struct ttf_font *font,
                   int x, int baseline, const char *text,
                   int px_size, u32 color);

/* Cell width for laying this font out on a fixed grid (a console). Based on
 * the digit advance, not the widest glyph — see the note in ttf.c. */
int  ttf_cell_width(struct ttf_font *font, int px_size);

/* Draw one codepoint centred in a cell_w-wide cell, condensing it if it is
 * wider than the cell. The counterpart to ttf_cell_width. */
void ttf_draw_glyph_cell(struct gfx_surface *s, struct ttf_font *font,
                         int cell_x, int baseline, int cell_w, int cp,
                         int px_size, u32 color);

#endif /* TTF_H */
