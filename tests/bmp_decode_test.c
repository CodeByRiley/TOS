/* Host-side regression test for userspace/lib/bmp.c.
 *
 * Decodes the generated cursor bitmap and checks it comes back as the
 * exact sprite tools/make_cursor.py encoded , same dimensions, same
 * pixels, right way up. A cursor that decodes upside down or with red and
 * blue swapped still renders something, so "it drew" is not evidence; the
 * pixel comparison is.
 *
 * Also covers the paths a hand-authored file is likely to take: 24-bit
 * BI_RGB, top-down row order, and a 32-bit image whose alpha channel is
 * all zeroes (which must be promoted to opaque, not treated as invisible).
 *
 * bmp.c calls the TOS syscall wrappers; this file supplies host-backed
 * definitions of exactly those four, so no part of lib/syscall.c is
 * needed.
 *
 * Build:
 *   gcc -I userspace/lib -o bmp_test tests/bmp_decode_test.c \
 *       userspace/lib/bmp.c
 */
#include "bmp.h"
#include "syscall.h"
#include <string.h>

/* host-backed stand-ins for the syscalls bmp.c uses
 * Only these four are needed, so none of lib/syscall.c is linked.
 *
 * Host <stdio.h> cannot be included: it declares unlink() returning int
 * while syscall.h declares it returning long. Declaring the handful of
 * stdio functions used here sidesteps that; FILE is left opaque, since C
 * linkage does not care about the tag. */
struct _iobuf;
typedef struct _iobuf HFILE;

extern HFILE *fopen(const char *path, const char *mode);
extern size_t fread(void *buf, size_t sz, size_t n, HFILE *fp);
extern size_t fwrite(const void *buf, size_t sz, size_t n, HFILE *fp);
extern int    fclose(HFILE *fp);
extern int    fseek(HFILE *fp, long off, int whence);
extern long   ftell(HFILE *fp);
extern int    printf(const char *fmt, ...);
extern int    remove(const char *path);

#define SEEK_SET 0
#define SEEK_END 2

#define MAX_FDS 8
static HFILE *host_files[MAX_FDS];

long open(const char *path, int flags) {
    (void)flags;
    for (int i = 3; i < MAX_FDS; i++) {
        if (!host_files[i]) {
            HFILE *fp = fopen(path, "rb");
            if (!fp) return -1;
            host_files[i] = fp;
            return i;
        }
    }
    return -1;
}

long close(int fd) {
    if (fd < 3 || fd >= MAX_FDS || !host_files[fd]) return -1;
    fclose(host_files[fd]);
    host_files[fd] = 0;
    return 0;
}

long read(int fd, void *buf, size_t n) {
    if (fd < 3 || fd >= MAX_FDS || !host_files[fd]) return -1;
    return (long)fread(buf, 1, n, host_files[fd]);
}

long fstat_raw(int fd, struct stat_user *out) {
    if (fd < 3 || fd >= MAX_FDS || !host_files[fd]) return -1;
    long here = ftell(host_files[fd]);
    fseek(host_files[fd], 0, SEEK_END);
    long end = ftell(host_files[fd]);
    fseek(host_files[fd], here, SEEK_SET);
    out->size = (unsigned long long)end;
    out->first_cluster = 0;
    out->type = STAT_TYPE_FILE;
    out->attr = 0;
    return 0;
}

/* expectations */

#define W 12
#define H 12

static const unsigned char mask[H][W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,1,1,1,1,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

static int failed = 0;

static void expect(int cond, const char *what) {
    if (cond) return;
    printf("FAIL: %s\n", what);
    failed = 1;
}

/* synthetic images for the shapes an editor might emit */

static void put32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v); p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* 2x2, 24-bit BI_RGB. Rows are 6 bytes padded to 8. */
static void write_bmp24(const char *path, int top_down) {
    unsigned char f[14 + 40 + 16];
    memset(f, 0, sizeof(f));
    f[0] = 'B'; f[1] = 'M';
    put32(f + 2, sizeof(f));
    put32(f + 10, 54);
    put32(f + 14, 40);
    put32(f + 18, 2);
    put32(f + 22, (unsigned int)(top_down ? -2 : 2));
    f[26] = 1; f[28] = 24;
    put32(f + 30, 0);

    /* Both variants store identical bytes and differ only in the sign of
     * the height, so the two decodes must come out vertically mirrored.
     * Writing different bytes per variant would let a decoder that
     * ignores the sign pass both. 24bpp is stored B,G,R. */
    unsigned char *px = f + 54;
    unsigned char first[8]  = {0,0,255, 0,255,0, 0,0};      /* red,  green */
    unsigned char second[8] = {255,0,0, 255,255,255, 0,0};  /* blue, white */
    memcpy(px,     first,  8);
    memcpy(px + 8, second, 8);

    HFILE *fp = fopen(path, "wb");
    fwrite(f, 1, sizeof(f), fp);
    fclose(fp);
}

/* 1x1, 32-bit BI_RGB with a zero alpha byte , the "no alpha authored"
 * case that must not decode as invisible. */
static void write_bmp32_zero_alpha(const char *path) {
    unsigned char f[14 + 40 + 4];
    memset(f, 0, sizeof(f));
    f[0] = 'B'; f[1] = 'M';
    put32(f + 2, sizeof(f));
    put32(f + 10, 54);
    put32(f + 14, 40);
    put32(f + 18, 1);
    put32(f + 22, 1);
    f[26] = 1; f[28] = 32;
    put32(f + 30, 0);
    put32(f + 54, 0x00204060u);   /* alpha 0, r=0x20 g=0x40 b=0x60 */

    HFILE *fp = fopen(path, "wb");
    fwrite(f, 1, sizeof(f), fp);
    fclose(fp);
}

int main(int argc, char **argv) {
    const char *cursor = argc > 1 ? argv[1] : "rootfs/system/icons/cursor.bmp";

    struct bmp_image img;
    memset(&img, 0, sizeof(img));

    expect(bmp_load(cursor, &img) == 0, "decode the cursor bitmap");
    if (!img.pixels) {
        printf("cannot continue without the cursor image\n");
        return 1;
    }

    expect(img.width == W && img.height == H, "cursor is 12x12");

    int mismatches = 0;
    for (int y = 0; y < H && y < img.height; y++) {
        for (int x = 0; x < W && x < img.width; x++) {
            unsigned int got = img.pixels[y * img.width + x];
            unsigned int want = mask[y][x] == 0 ? 0x00000000u
                              : mask[y][x] == 1 ? 0xFF000000u
                                                : 0xFFFFFFFFu;
            if (got != want) {
                if (mismatches < 4)
                    printf("  (%d,%d) got %08x want %08x\n", x, y, got, want);
                mismatches++;
            }
        }
    }
    expect(mismatches == 0, "every cursor pixel matches the source mask");

    /* Row order is the easiest thing to get backwards, and the arrow is
     * asymmetric enough to catch it: the tip is opaque, the far corner is
     * not. */
    expect((img.pixels[0] >> 24) == 0xFF, "top-left is the opaque tip");
    expect((img.pixels[(H - 1) * W + (W - 1)] >> 24) == 0x00,
           "bottom-right is transparent");

    bmp_free(&img);
    expect(img.pixels == 0, "bmp_free clears the struct");

    /* 24-bit, both row orders */
    write_bmp24("bmp24_bu.bmp", 0);
    memset(&img, 0, sizeof(img));
    expect(bmp_load("bmp24_bu.bmp", &img) == 0, "decode 24-bit bottom-up");
    if (img.pixels) {
        /* Bottom-up: the first stored row is the bottom one, so the blue
         * row ends up on top. */
        expect(img.pixels[0] == 0xFF0000FFu, "bottom-up puts blue on top");
        expect(img.pixels[2] == 0xFFFF0000u, "bottom-up puts red below");
        expect((img.pixels[0] >> 24) == 0xFF, "24-bit decodes opaque");
        bmp_free(&img);
    }

    write_bmp24("bmp24_td.bmp", 1);
    memset(&img, 0, sizeof(img));
    expect(bmp_load("bmp24_td.bmp", &img) == 0, "decode 24-bit top-down");
    if (img.pixels) {
        /* Same bytes, negative height: the red row is now the top one. */
        expect(img.pixels[0] == 0xFFFF0000u, "top-down puts red on top");
        expect(img.pixels[2] == 0xFF0000FFu, "top-down puts blue below");
        bmp_free(&img);
    }

    /* 32-bit with an unwritten alpha channel */
    write_bmp32_zero_alpha("bmp32_noalpha.bmp");
    memset(&img, 0, sizeof(img));
    expect(bmp_load("bmp32_noalpha.bmp", &img) == 0, "decode 32-bit BI_RGB");
    if (img.pixels) {
        expect(img.pixels[0] == 0xFF204060u,
               "all-zero alpha is promoted to opaque");
        bmp_free(&img);
    }

    /* rejections */
    memset(&img, 0, sizeof(img));
    expect(bmp_load("does_not_exist.bmp", &img) != 0, "missing file fails");
    expect(img.pixels == 0, "failed load leaves the caller's image alone");

    remove("bmp24_bu.bmp");
    remove("bmp24_td.bmp");
    remove("bmp32_noalpha.bmp");

    if (!failed) printf("bmp_decode_test: all checks passed\n");
    return failed;
}
