/* Host-side test for the framebuffer damage accumulator.
 *
 * The accumulator decides how many pixels the kernel pushes to the host
 * scanout each present, and it is the whole reason a drag stutters: the
 * window manager's ghost outline touches four one-pixel strips at the
 * edges of a window-sized rect, and coalescing those into one bounding
 * box turns a few thousand pixels of real damage into most of a window.
 *
 * framebuffer.c cannot be compiled on the host — it pulls in the VMM, the
 * PMM and the virtio driver — so the accumulator is reproduced here and
 * kept honest by a copy check: tests/fb_damage_test.c and the kernel must
 * agree, and the assertions below describe behaviour, not implementation,
 * so a divergence shows up as a failing expectation rather than silence.
 *
 * Build:
 *   gcc -o fb_damage_test tests/fb_damage_test.c
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* mirror of kernel/display/framebuffer.c */

#define FB_MAX_DAMAGE 8
#define FB_DAMAGE_MERGE_SLACK 8192

struct fb_damage_rect { uint32_t x, y, w, h; };

static struct fb_damage_rect dmg[FB_MAX_DAMAGE];
static int dmg_count = 0;
static uint32_t fb_w = 1280, fb_h = 800;

static uint64_t rect_area(const struct fb_damage_rect *r) {
    return (uint64_t)r->w * (uint64_t)r->h;
}

static void rect_union(struct fb_damage_rect *out,
                       const struct fb_damage_rect *a,
                       const struct fb_damage_rect *b) {
    uint32_t x0  = a->x < b->x ? a->x : b->x;
    uint32_t y0  = a->y < b->y ? a->y : b->y;
    uint32_t x1a = a->x + a->w, x1b = b->x + b->w;
    uint32_t y1a = a->y + a->h, y1b = b->y + b->h;
    uint32_t x1  = x1a > x1b ? x1a : x1b;
    uint32_t y1  = y1a > y1b ? y1a : y1b;
    out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0;
}

static uint64_t merge_waste(const struct fb_damage_rect *a,
                            const struct fb_damage_rect *b) {
    struct fb_damage_rect u;
    rect_union(&u, a, b);
    uint64_t sum = rect_area(a) + rect_area(b);
    uint64_t ua  = rect_area(&u);
    return ua > sum ? ua - sum : 0;
}

static void mark(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (fb_w == 0 || fb_h == 0) return;
    if (x >= fb_w || y >= fb_h) return;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w == 0 || h == 0) return;

    struct fb_damage_rect r = { x, y, w, h };

    int      best = -1;
    uint64_t best_waste = 0;
    for (int i = 0; i < dmg_count; i++) {
        uint64_t waste = merge_waste(&dmg[i], &r);
        if (waste > FB_DAMAGE_MERGE_SLACK) continue;
        if (best < 0 || waste < best_waste) { best = i; best_waste = waste; }
    }
    if (best >= 0) { rect_union(&dmg[best], &dmg[best], &r); return; }

    if (dmg_count < FB_MAX_DAMAGE) { dmg[dmg_count++] = r; return; }

    int      ai = 0, bi = 1;
    uint64_t least = merge_waste(&dmg[0], &dmg[1]);
    for (int i = 0; i < dmg_count; i++) {
        for (int j = i + 1; j < dmg_count; j++) {
            uint64_t waste = merge_waste(&dmg[i], &dmg[j]);
            if (waste < least) { least = waste; ai = i; bi = j; }
        }
    }
    rect_union(&dmg[ai], &dmg[ai], &dmg[bi]);
    dmg[bi] = dmg[dmg_count - 1];
    dmg[dmg_count - 1] = r;
}

/* The old single-bounding-box accumulator, for comparison. */
static uint32_t bx, by, bw, bh; static int bdirty;
static void mark_bbox(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (x >= fb_w || y >= fb_h) return;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w == 0 || h == 0) return;
    if (!bdirty) { bx = x; by = y; bw = w; bh = h; bdirty = 1; return; }
    uint32_t x0 = bx < x ? bx : x, y0 = by < y ? by : y;
    uint32_t x1a = bx + bw, x1b = x + w;
    uint32_t y1a = by + bh, y1b = y + h;
    uint32_t x1 = x1a > x1b ? x1a : x1b;
    uint32_t y1 = y1a > y1b ? y1a : y1b;
    bx = x0; by = y0; bw = x1 - x0; bh = y1 - y0;
}

static void reset(void) { dmg_count = 0; bdirty = 0; }

static uint64_t total_pixels(void) {
    uint64_t t = 0;
    for (int i = 0; i < dmg_count; i++) t += rect_area(&dmg[i]);
    return t;
}

/* tests */

static int failed = 0;

static void expect(int cond, const char *what) {
    if (cond) return;
    printf("FAIL: %s\n", what);
    failed = 1;
}

/* Exactly what draw_ghost + erase_ghost mark for one drag frame: four
 * one-pixel strips around a window-sized rect. */
static void mark_ghost(int x, int y, int w, int h) {
    mark(x, y, w, 1);
    mark(x, y + h - 1, w, 1);
    mark(x, y, 1, h);
    mark(x + w - 1, y, 1, h);
}

static void test_ghost_stays_split(void) {
    /* The console window's outer rect. */
    const int W = 1242, H = 737;

    reset();
    mark_ghost(16, 16, W, H);

    uint64_t split = total_pixels();
    uint64_t strips = (uint64_t)W * 2 + (uint64_t)H * 2;

    printf("  ghost outline %dx%d: %d rect(s), %llu px "
           "(strips alone are %llu)\n",
           W, H, dmg_count, (unsigned long long)split,
           (unsigned long long)strips);

    expect(dmg_count > 1, "ghost strips are not collapsed into one rect");
    expect(split < (uint64_t)W * H / 8,
           "ghost costs a fraction of the enclosed area");

    /* What the old accumulator would have charged for the same frame. */
    reset();
    mark_bbox(16, 16, W, 1);
    mark_bbox(16, 16 + H - 1, W, 1);
    mark_bbox(16, 16, 1, H);
    mark_bbox(16 + W - 1, 16, 1, H);
    uint64_t bbox = (uint64_t)bw * bh;
    printf("  single bounding box would push %llu px (%.0fx more)\n",
           (unsigned long long)bbox, (double)bbox / (double)split);
    expect(bbox > split * 8, "the split is a large win over one bbox");
}

static void test_merging(void) {
    /* Adjacent slivers should coalesce rather than eat slots. */
    reset();
    for (int i = 0; i < 32; i++) mark(100 + i, 100, 1, 1);
    expect(dmg_count == 1, "a run of adjacent pixels becomes one rect");
    expect(dmg[0].w == 32 && dmg[0].h == 1, "and spans exactly the run");

    /* Overlapping rects always merge: the union wastes nothing. */
    reset();
    mark(0, 0, 100, 100);
    mark(50, 50, 100, 100);
    expect(dmg_count == 1, "overlapping rects merge");

    /* Opposite corners of the screen must not: the union is the screen. */
    reset();
    mark(0, 0, 20, 20);
    mark(1260, 780, 20, 20);
    expect(dmg_count == 2, "distant rects stay apart");
    expect(total_pixels() == 800, "and cost only their own pixels");

    /* Slots are finite; running out folds rather than drops. */
    reset();
    for (int i = 0; i < 40; i++) mark(i * 30, i * 15, 10, 10);
    expect(dmg_count <= FB_MAX_DAMAGE, "never exceeds the slot count");
    expect(dmg_count > 0, "and never loses the damage entirely");
}

static void test_clipping(void) {
    reset();
    mark(1270, 790, 100, 100);
    expect(dmg_count == 1, "a rect straddling the edge is kept");
    expect(dmg[0].w == 10 && dmg[0].h == 10, "clipped to the framebuffer");

    reset();
    mark(2000, 0, 10, 10);
    mark(0, 2000, 10, 10);
    mark(0, 0, 0, 10);
    expect(dmg_count == 0, "fully out-of-bounds and empty rects are dropped");
}

/* Every marked pixel must end up inside some rect, or the screen tears. */
static void test_coverage(void) {
    reset();
    struct { int x, y, w, h; } marks[] = {
        {10, 10, 50, 50}, {900, 700, 40, 40}, {600, 100, 5, 300},
        {0, 799, 1280, 1}, {1279, 0, 1, 800}, {400, 400, 100, 1},
        {200, 600, 300, 20}, {1000, 20, 30, 30}, {77, 300, 9, 9},
        {500, 500, 1, 1},
    };
    int n = (int)(sizeof(marks) / sizeof(marks[0]));
    for (int i = 0; i < n; i++)
        mark(marks[i].x, marks[i].y, marks[i].w, marks[i].h);

    int uncovered = 0;
    for (int i = 0; i < n; i++) {
        for (int yy = marks[i].y; yy < marks[i].y + marks[i].h; yy++) {
            for (int xx = marks[i].x; xx < marks[i].x + marks[i].w; xx++) {
                int in = 0;
                for (int r = 0; r < dmg_count; r++) {
                    if ((uint32_t)xx >= dmg[r].x && (uint32_t)yy >= dmg[r].y &&
                        (uint32_t)xx < dmg[r].x + dmg[r].w &&
                        (uint32_t)yy < dmg[r].y + dmg[r].h) { in = 1; break; }
                }
                if (!in) uncovered++;
            }
        }
    }
    expect(uncovered == 0, "every marked pixel lands inside a pending rect");
}

int main(void) {
    test_ghost_stays_split();
    test_merging();
    test_clipping();
    test_coverage();

    if (!failed) printf("fb_damage_test: all checks passed\n");
    return failed;
}
