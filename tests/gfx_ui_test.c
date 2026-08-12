/* Host-side regression test for userspace/lib/gfx.c and lib/ui.c.
 *
 * Both are pure memory arithmetic — no syscalls beyond the file read
 * behind gfx_sprite_load — so everything here runs on the host and checks
 * actual pixels rather than "it didn't crash".
 *
 * The clipping cases matter most. Every drawing entry point promises a
 * caller can pass coordinates that fall outside the target, which is only
 * worth anything if out-of-bounds writes really are suppressed; the
 * surfaces below are allocated with a sentinel margin so a stray write
 * shows up as a changed guard pixel rather than as silence.
 *
 * Build:
 *   gcc -I userspace/lib -o gfx_test tests/gfx_ui_test.c \
 *       userspace/lib/gfx.c userspace/lib/ui.c userspace/lib/bmp.c
 */
#include "gfx.h"
#include "ui.h"
#include "syscall.h"
#include "../userspace/include/font8x8.h"
#include <stdlib.h>
#include <string.h>

extern int printf(const char *fmt, ...);

/* bmp.c reaches for these; the test never loads a real file, so open()
 * failing is the sprite-fallback case under test. */
long open(const char *path, int flags)         { (void)path; (void)flags; return -1; }
long close(int fd)                             { (void)fd; return -1; }
long read(int fd, void *b, size_t n)           { (void)fd; (void)b; (void)n; return -1; }
long fstat_raw(int fd, struct stat_user *o)    { (void)fd; (void)o; return -1; }

static int failed = 0;

static void expect(int cond, const char *what) {
    if (cond) return;
    printf("FAIL: %s\n", what);
    failed = 1;
}

/* surface with guard margin */

#define GUARD 4
#define SENTINEL 0xDEADBEEFu

struct guarded {
    uint32_t          *base;      /* full allocation, including margin */
    int                total_w, total_h;
    struct gfx_surface s;         /* view onto the interior            */
};

static void guarded_init(struct guarded *g, int w, int h) {
    g->total_w = w + 2 * GUARD;
    g->total_h = h + 2 * GUARD;
    g->base = malloc((size_t)g->total_w * g->total_h * sizeof(uint32_t));
    for (int i = 0; i < g->total_w * g->total_h; i++) g->base[i] = SENTINEL;

    uint32_t *inner = g->base + (size_t)GUARD * g->total_w + GUARD;
    gfx_surface_init(&g->s, inner, w, h, g->total_w);
    gfx_clear(&g->s, 0);
}

/* Every pixel outside the surface must still hold the sentinel. */
static int guard_intact(const struct guarded *g) {
    for (int y = 0; y < g->total_h; y++) {
        for (int x = 0; x < g->total_w; x++) {
            int inside = x >= GUARD && y >= GUARD
                      && x < GUARD + g->s.w && y < GUARD + g->s.h;
            if (inside) continue;
            if (g->base[(size_t)y * g->total_w + x] != SENTINEL) return 0;
        }
    }
    return 1;
}

static uint32_t px_at(const struct guarded *g, int x, int y) {
    return g->s.px[(size_t)y * g->s.stride + x];
}

static void guarded_free(struct guarded *g) { free(g->base); }

/* rectangles */

static void test_rects(void) {
    struct gfx_rect a = gfx_rect_make(0, 0, 10, 10);
    struct gfx_rect b = gfx_rect_make(5, 5, 10, 10);

    struct gfx_rect i = gfx_rect_intersect(a, b);
    expect(i.x == 5 && i.y == 5 && i.w == 5 && i.h == 5, "overlap");

    struct gfx_rect miss = gfx_rect_intersect(a, gfx_rect_make(50, 50, 4, 4));
    expect(gfx_rect_empty(miss), "disjoint rects give an empty overlap");

    struct gfx_rect in = gfx_rect_inset(a, 2);
    expect(in.x == 2 && in.y == 2 && in.w == 6 && in.h == 6, "inset");
    expect(gfx_rect_empty(gfx_rect_inset(a, 8)), "over-inset goes empty");

    expect(gfx_rect_contains(a, 0, 0), "contains its top-left");
    expect(!gfx_rect_contains(a, 10, 0), "excludes its right edge");
    expect(!gfx_rect_contains(a, 0, 10), "excludes its bottom edge");
}

/* fills and clipping */

static void test_fill_clip(void) {
    struct guarded g;
    guarded_init(&g, 16, 16);

    gfx_fill(&g.s, gfx_rect_make(2, 2, 4, 4), 0x00112233u);
    expect(px_at(&g, 2, 2) == 0x00112233u, "fill writes its top-left");
    expect(px_at(&g, 5, 5) == 0x00112233u, "fill writes its bottom-right");
    expect(px_at(&g, 6, 6) == 0, "fill stops at its edge");

    /* Straddling every boundary at once. */
    gfx_fill(&g.s, gfx_rect_make(-4, -4, 100, 100), 0x00445566u);
    expect(px_at(&g, 0, 0) == 0x00445566u, "oversized fill covers origin");
    expect(px_at(&g, 15, 15) == 0x00445566u, "oversized fill covers corner");
    expect(guard_intact(&g), "oversized fill wrote nothing out of bounds");

    /* Entirely outside, in each direction. */
    gfx_fill(&g.s, gfx_rect_make(-50, 0, 10, 10), 0x00FF0000u);
    gfx_fill(&g.s, gfx_rect_make(100, 0, 10, 10), 0x00FF0000u);
    gfx_fill(&g.s, gfx_rect_make(0, -50, 10, 10), 0x00FF0000u);
    gfx_fill(&g.s, gfx_rect_make(0, 100, 10, 10), 0x00FF0000u);
    expect(guard_intact(&g), "off-surface fills write nothing");
    expect(px_at(&g, 0, 0) == 0x00445566u, "off-surface fills left pixels be");

    /* Degenerate sizes. */
    gfx_clear(&g.s, 0);
    gfx_fill(&g.s, gfx_rect_make(4, 4, 0, 5), 0x00FF0000u);
    gfx_fill(&g.s, gfx_rect_make(4, 4, 5, -3), 0x00FF0000u);
    expect(px_at(&g, 4, 4) == 0, "zero and negative extents draw nothing");

    guarded_free(&g);
}

static void test_clip_stack(void) {
    struct guarded g;
    guarded_init(&g, 16, 16);

    struct gfx_rect prev = gfx_clip_push(&g.s, gfx_rect_make(4, 4, 4, 4));
    gfx_fill(&g.s, gfx_rect_make(0, 0, 16, 16), 0x00AAAAAAu);
    expect(px_at(&g, 3, 3) == 0, "clip keeps the fill out above-left");
    expect(px_at(&g, 4, 4) == 0x00AAAAAAu, "clip admits its own area");
    expect(px_at(&g, 8, 8) == 0, "clip keeps the fill out below-right");

    /* Pushing again narrows further; it never widens. */
    gfx_clip_push(&g.s, gfx_rect_make(0, 0, 16, 16));
    gfx_fill(&g.s, gfx_rect_make(0, 0, 16, 16), 0x00BBBBBBu);
    expect(px_at(&g, 3, 3) == 0, "a wider push cannot widen the clip");
    expect(px_at(&g, 4, 4) == 0x00BBBBBBu, "a wider push keeps the old area");

    gfx_clip_set(&g.s, prev);
    gfx_fill(&g.s, gfx_rect_make(0, 0, 16, 16), 0x00CCCCCCu);
    expect(px_at(&g, 0, 0) == 0x00CCCCCCu, "restoring the clip reopens it");

    guarded_free(&g);
}

/* blending */

static void test_blend(void) {
    struct guarded g;
    guarded_init(&g, 8, 8);
    gfx_clear(&g.s, 0x00000000u);

    gfx_blend(&g.s, 1, 1, 0x00FFFFFFu);
    expect(px_at(&g, 1, 1) == 0, "alpha 0 leaves the pixel alone");

    gfx_blend(&g.s, 2, 2, 0xFFFFFFFFu);
    expect(px_at(&g, 2, 2) == 0x00FFFFFFu, "alpha 255 replaces the pixel");

    /* Half of white over black lands mid-grey, allowing for rounding. */
    gfx_blend(&g.s, 3, 3, 0x80FFFFFFu);
    uint32_t mid = px_at(&g, 3, 3);
    int r = (mid >> 16) & 0xFF;
    expect(r >= 126 && r <= 130, "half alpha blends halfway");

    gfx_blend(&g.s, -1, -1, 0xFFFFFFFFu);
    gfx_blend(&g.s, 99, 99, 0xFFFFFFFFu);
    expect(guard_intact(&g), "off-surface blends write nothing");

    guarded_free(&g);
}

/* blits */

static void test_blit(void) {
    struct guarded dst;
    guarded_init(&dst, 16, 16);

    uint32_t srcpx[4 * 4];
    for (int i = 0; i < 16; i++) srcpx[i] = 0xFF000000u | (uint32_t)i;
    struct gfx_surface src;
    gfx_surface_init(&src, srcpx, 4, 4, 4);

    gfx_blit(&dst.s, 2, 2, &src, gfx_rect_make(0, 0, 0, 0));
    expect(px_at(&dst, 2, 2) == 0xFF000000u, "empty src rect blits it all");
    expect(px_at(&dst, 5, 5) == 0xFF00000Fu, "blit lands the last pixel");

    /* A sub-rect must take its pixels from the right place. */
    gfx_clear(&dst.s, 0);
    gfx_blit(&dst.s, 0, 0, &src, gfx_rect_make(1, 1, 2, 2));
    expect(px_at(&dst, 0, 0) == 0xFF000005u, "sub-rect blit picks its origin");

    /* Clipped at the top-left: the source has to advance to match. */
    gfx_clear(&dst.s, 0);
    gfx_blit(&dst.s, -2, -2, &src, gfx_rect_make(0, 0, 0, 0));
    expect(px_at(&dst, 0, 0) == 0xFF00000Au,
           "clipping a blit offsets the source too");
    expect(guard_intact(&dst), "negatively placed blit stays in bounds");

    gfx_blit(&dst.s, 200, 200, &src, gfx_rect_make(0, 0, 0, 0));
    expect(guard_intact(&dst), "off-surface blit writes nothing");

    /* Alpha variant: zero-alpha source pixels leave the destination. */
    gfx_clear(&dst.s, 0x00222222u);
    uint32_t apx[4] = { 0xFFFF0000u, 0x00FF0000u, 0xFFFF0000u, 0x00FF0000u };
    struct gfx_surface asrc;
    gfx_surface_init(&asrc, apx, 2, 2, 2);
    gfx_blit_alpha(&dst.s, 0, 0, &asrc, gfx_rect_make(0, 0, 0, 0));
    expect(px_at(&dst, 0, 0) == 0x00FF0000u, "opaque source pixel lands");
    expect(px_at(&dst, 1, 0) == 0x00222222u, "transparent source pixel skipped");

    /* 2x magnification duplicates each source pixel into a 2x2 block. */
    gfx_clear(&dst.s, 0);
    gfx_blit_scaled(&dst.s, 0, 0, &src, gfx_rect_make(0, 0, 2, 2), 2, 0);
    expect(px_at(&dst, 0, 0) == 0x00000000u &&
           px_at(&dst, 1, 1) == 0x00000000u, "scaled blit fills 2x2 from px 0");
    expect(px_at(&dst, 2, 0) == 0x00000001u &&
           px_at(&dst, 3, 1) == 0x00000001u, "scaled blit advances by scale");
    expect(guard_intact(&dst), "scaled blit stays in bounds");

    guarded_free(&dst);
}

/* text */

static void test_text(void) {
    int w, h;
    gfx_text_size("abc", 1, &w, &h);
    expect(w == 24 && h == 8, "text size at scale 1");
    gfx_text_size("abc", 2, &w, &h);
    expect(w == 48 && h == 16, "text size at scale 2");
    gfx_text_size("", 1, &w, &h);
    expect(w == 0, "empty string measures zero");

    expect(gfx_text_fit("abcdef", 1, 24) == 3, "fit counts whole glyphs");
    expect(gfx_text_fit("abcdef", 1, 23) == 2, "a partial glyph does not fit");
    expect(gfx_text_fit("abcdef", 2, 32) == 2, "fit accounts for scale");
    expect(gfx_text_fit("abc", 1, 0) == 0, "nothing fits in no width");

    struct guarded g;
    guarded_init(&g, 32, 16);

    /* Compare against the font table directly: whatever the renderer
     * does, the lit pixels must be exactly the glyph's set bits. */
    gfx_text(&g.s, 0, 0, "A", 0x00FFFFFFu, 1);
    const uint8_t *rows = font8x8['A' - FONT_FIRST];
    int mismatches = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int want_lit = (rows[y] >> x) & 1;
            int got_lit  = px_at(&g, x, y) == 0x00FFFFFFu;
            if (want_lit != got_lit) mismatches++;
        }
    expect(mismatches == 0, "glyph pixels match the font table");

    /* Unprintables must not draw and must not run off the table. */
    gfx_clear(&g.s, 0);
    gfx_text(&g.s, 0, 0, "\x01\x7F", 0x00FFFFFFu, 1);
    int lit = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++)
            if (px_at(&g, x, y)) lit++;
    expect(lit == 0, "characters outside the font draw nothing");

    /* Background variant paints the whole cell. */
    gfx_clear(&g.s, 0);
    gfx_text_bg(&g.s, 0, 0, " ", 0x00FFFFFFu, 0x00333333u, 1);
    expect(px_at(&g, 0, 0) == 0x00333333u && px_at(&g, 7, 7) == 0x00333333u,
           "text background fills the cell");

    /* Text running off the edge is clipped, not wrapped. */
    gfx_clear(&g.s, 0);
    gfx_text(&g.s, 28, 0, "AAAA", 0x00FFFFFFu, 1);
    expect(guard_intact(&g), "text past the edge stays in bounds");

    guarded_free(&g);
}

/* masks and sprites */

static void test_masks(void) {
    struct guarded g;
    guarded_init(&g, 16, 16);

    static const uint8_t m[2 * 3] = { 1, 0, 0, 2, 1, 2 };

    gfx_mask(&g.s, 0, 0, m, 3, 2, 0x00FF0000u, 1);
    expect(px_at(&g, 0, 0) == 0x00FF0000u, "mask stamps where it is set");
    expect(px_at(&g, 1, 0) == 0, "mask leaves zero bytes alone");
    expect(px_at(&g, 0, 1) == 0x00FF0000u, "every non-zero value stamps");

    gfx_clear(&g.s, 0);
    uint32_t colors[2] = { 0x00111111u, 0x00222222u };
    gfx_mask_multi(&g.s, 0, 0, m, 3, 2, colors, 2, 1);
    expect(px_at(&g, 0, 0) == 0x00111111u, "value 1 takes the first colour");
    expect(px_at(&g, 0, 1) == 0x00222222u, "value 2 takes the second");

    /* Values past the table are skipped rather than read out of bounds. */
    gfx_clear(&g.s, 0);
    gfx_mask_multi(&g.s, 0, 0, m, 3, 2, colors, 1, 1);
    expect(px_at(&g, 0, 0) == 0x00111111u, "in-range value still stamps");
    expect(px_at(&g, 0, 1) == 0, "out-of-range value is skipped");

    gfx_clear(&g.s, 0);
    gfx_mask(&g.s, 0, 0, m, 3, 2, 0x00FF0000u, 3);
    expect(px_at(&g, 2, 2) == 0x00FF0000u, "scaled mask fills its block");
    expect(px_at(&g, 3, 0) == 0, "scaled mask respects zero bytes");

    gfx_mask(&g.s, -10, -10, m, 3, 2, 0x00FF0000u, 1);
    gfx_mask(&g.s, 100, 100, m, 3, 2, 0x00FF0000u, 1);
    expect(guard_intact(&g), "off-surface masks write nothing");

    /* A sprite with no loadable file falls back to its mask. */
    struct gfx_sprite sp;
    memset(&sp, 0, sizeof(sp));
    gfx_sprite_set_mask(&sp, m, 3, 2, colors, 2);
    expect(gfx_sprite_w(&sp) == 3 && gfx_sprite_h(&sp) == 2,
           "sprite reports its mask size");
    expect(gfx_sprite_load(&sp, "/nope.bmp") != 0, "missing sprite file fails");
    expect(sp.image.pixels == 0, "a failed load leaves the fallback in place");

    gfx_clear(&g.s, 0);
    gfx_sprite_draw(&g.s, 0, 0, &sp, 1);
    expect(px_at(&g, 0, 0) == 0x00111111u, "sprite draws through its fallback");

    guarded_free(&g);
}

/* ui */

/* One frame with the given pointer state. */
static void frame(struct ui_context *c, struct gfx_surface *s,
                  int x, int y, int down) {
    ui_begin(c, s, 0, x, y, down ? MOUSE_BTN_LEFT : 0);
}

static void test_ui_button(void) {
    struct guarded g;
    guarded_init(&g, 64, 32);

    struct ui_context ui;
    memset(&ui, 0, sizeof(ui));
    struct gfx_rect r = gfx_rect_make(4, 4, 40, 16);

    /* Hovering is not clicking. */
    frame(&ui, &g.s, 10, 10, 0);
    expect(ui_button(&ui, r, "Go") == 0, "hover alone does not click");
    ui_end(&ui);

    /* Press, then release inside: one click, on the release. */
    frame(&ui, &g.s, 10, 10, 1);
    expect(ui_button(&ui, r, "Go") == 0, "press alone does not click");
    ui_end(&ui);

    frame(&ui, &g.s, 10, 10, 0);
    expect(ui_button(&ui, r, "Go") == 1, "release inside clicks");
    ui_end(&ui);

    /* And only once. */
    frame(&ui, &g.s, 10, 10, 0);
    expect(ui_button(&ui, r, "Go") == 0, "the click does not repeat");
    ui_end(&ui);

    /* Press inside, drag out, release: cancelled. */
    frame(&ui, &g.s, 10, 10, 1);
    ui_button(&ui, r, "Go");
    ui_end(&ui);
    frame(&ui, &g.s, 60, 28, 1);
    ui_button(&ui, r, "Go");
    ui_end(&ui);
    frame(&ui, &g.s, 60, 28, 0);
    expect(ui_button(&ui, r, "Go") == 0, "releasing outside cancels");
    ui_end(&ui);

    /* A press that began off the widget must not arm it on entry. */
    frame(&ui, &g.s, 60, 28, 1);
    ui_button(&ui, r, "Go");
    ui_end(&ui);
    frame(&ui, &g.s, 10, 10, 1);
    ui_button(&ui, r, "Go");
    ui_end(&ui);
    frame(&ui, &g.s, 10, 10, 0);
    expect(ui_button(&ui, r, "Go") == 0,
           "a press begun elsewhere does not click on release");
    ui_end(&ui);

    expect(guard_intact(&g), "button drawing stays in bounds");
    guarded_free(&g);
}

static void test_ui_widgets(void) {
    struct guarded g;
    guarded_init(&g, 64, 64);

    struct ui_context ui;
    memset(&ui, 0, sizeof(ui));
    struct gfx_rect r = gfx_rect_make(0, 0, 60, 16);
    int checked = 0;

    frame(&ui, &g.s, 5, 5, 1);
    ui_checkbox(&ui, r, "On", &checked);
    ui_end(&ui);
    frame(&ui, &g.s, 5, 5, 0);
    expect(ui_checkbox(&ui, r, "On", &checked) == 1, "checkbox reports change");
    ui_end(&ui);
    expect(checked == 1, "checkbox toggled on");

    frame(&ui, &g.s, 5, 5, 1);
    ui_checkbox(&ui, r, "On", &checked);
    ui_end(&ui);
    frame(&ui, &g.s, 5, 5, 0);
    ui_checkbox(&ui, r, "On", &checked);
    ui_end(&ui);
    expect(checked == 0, "checkbox toggled back off");

    /* Progress clamps rather than overdrawing. */
    frame(&ui, &g.s, 0, 0, 0);
    ui_progress(&ui, gfx_rect_make(0, 20, 60, 10), 250, 0);
    ui_progress(&ui, gfx_rect_make(0, 40, 60, 10), -40, 0);
    ui_end(&ui);
    expect(guard_intact(&g), "progress stays in bounds at absurd percentages");

    guarded_free(&g);
}

static void test_ui_layout(void) {
    struct ui_layout l;
    ui_layout_begin(&l, gfx_rect_make(10, 10, 100, 50), 4);

    struct gfx_rect a = ui_layout_row(&l, 20);
    expect(a.x == 10 && a.y == 10 && a.w == 100 && a.h == 20, "first row");

    struct gfx_rect b = ui_layout_row(&l, 20);
    expect(b.y == 34, "second row clears the first plus the gap");

    /* Past the bottom the rows go empty rather than overflowing. */
    struct gfx_rect c = ui_layout_row(&l, 20);
    expect(c.h <= 2, "a row past the area is truncated");
    struct gfx_rect d = ui_layout_row(&l, 20);
    expect(gfx_rect_empty(d), "rows past the area are empty");

    struct gfx_rect row = gfx_rect_make(0, 0, 100, 10);
    struct gfx_rect c0 = ui_layout_column(row, 3, 0, 5);
    struct gfx_rect c2 = ui_layout_column(row, 3, 2, 5);
    expect(c0.w == 30, "columns split the row minus the gaps");
    expect(c2.x == 70, "the last column starts after the others");
    expect(gfx_rect_empty(ui_layout_column(row, 3, 9, 5)),
           "an out-of-range column is empty");
}

int main(void) {
    test_rects();
    test_fill_clip();
    test_clip_stack();
    test_blend();
    test_blit();
    test_text();
    test_masks();
    test_ui_button();
    test_ui_widgets();
    test_ui_layout();

    if (!failed) printf("gfx_ui_test: all checks passed\n");
    return failed;
}
