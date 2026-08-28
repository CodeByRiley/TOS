/* userspace/lib/bmp.c , minimal BMP decoder.
 *
 * Handles the shapes an image editor actually emits for a small sprite:
 * BITMAPINFOHEADER through BITMAPV5HEADER, 24- or 32-bit, BI_RGB or
 * BI_BITFIELDS, stored bottom-up or top-down. Channel positions are read
 * from the bitfield masks rather than assumed, so an unusual byte order
 * decodes correctly instead of coming out with red and blue swapped.
 *
 * Everything is read into memory in one go , these are sprites, not
 * photographs, and the FAT driver has no readahead worth streaming for.
 */
#include "bmp.h"
#include <lib/syscall.h>

#ifdef _WIN32
/* Spelled out rather than pulled from <stdio.h> or <io.h>: every MinGW
 * header that defines SEEK_* also redeclares open/close/unlink with
 * prototypes that disagree with syscall.h's (int where it says long), and
 * syscall.h's are the ones this file is written against. The CRT satisfies
 * them at link time either way, and these two values are fixed. */
#define BMP_SEEK_SET 0
#define BMP_SEEK_END 2
#endif

extern void *malloc(size_t n);
extern void  free(void *p);

#define BI_RGB        0
#define BI_BITFIELDS  3

/* Little-endian reads. The images are authored on x86 and read on x86,
 * but going through these keeps the parser independent of the struct
 * padding rules the compiler would otherwise impose on a packed header. */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd32s(const uint8_t *p) {
    return (int32_t)rd32(p);
}

/* Bit position of a mask's lowest set bit, and how wide it is. A mask of
 * 0 means the channel is absent. */
static int mask_shift(uint32_t mask) {
    int s = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { mask >>= 1; s++; }
    return s;
}

static int mask_width(uint32_t mask) {
    int w = 0;
    if (!mask) return 0;
    while (!(mask & 1)) mask >>= 1;
    while (mask & 1) { mask >>= 1; w++; }
    return w;
}

/* Extract one channel and scale it to 0..255. Channels narrower than 8
 * bits are replicated rather than shifted, so a 5-bit 0x1F reaches 0xFF
 * instead of 0xF8. */
static uint32_t chan(uint32_t px, uint32_t mask, int shift, int width) {
    if (!mask) return 0;
    uint32_t v = (px & mask) >> shift;
    if (width == 8)  return v;
    if (width == 0)  return 0;
    if (width < 8)   return (v * 255u) / ((1u << width) - 1u);
    return v >> (width - 8);
}

/* Length of an open file, or -1.
 *
 * TOS answers this with fstat_raw. Nothing else has that syscall , holyd's
 * Windows build links this file for its font and sprite drawing , so there
 * the size comes from seeking to the end and back. */
static long file_size(int fd) {
#ifdef _WIN32
    long end = (long)lseek(fd, 0, BMP_SEEK_END);
    if (end < 0) return -1;
    if (lseek(fd, 0, BMP_SEEK_SET) < 0) return -1;
    return end;
#else
    struct stat_user st;
    if (fstat_raw(fd, &st) != 0) return -1;
    return (long)st.size;
#endif
}

static uint8_t *read_whole_file(const char *path, uint32_t *size_out) {
    long fd = open(path, 0);
    if (fd < 0) return 0;

    long size = file_size((int)fd);
    if (size <= 0) {
        close((int)fd);
        return 0;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        close((int)fd);
        return 0;
    }

    size_t got = 0;
    while (got < (size_t)size) {
        long n = read((int)fd, buf + got, (size_t)((size_t)size - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    close((int)fd);

    if (got != (size_t)size) {
        free(buf);
        return 0;
    }
    *size_out = (uint32_t)size;
    return buf;
}

int bmp_load(const char *path, struct bmp_image *out) {
    if (!path || !out) return -1;

    uint32_t size = 0;
    uint8_t *file = read_whole_file(path, &size);
    if (!file) return -1;

    int rc = -1;
    uint32_t *pixels = 0;

    /* File header: "BM", then the offset the pixel array starts at. */
    if (size < 54 || file[0] != 'B' || file[1] != 'M') goto done;

    uint32_t data_off = rd32(file + 10);
    uint32_t dib_size = rd32(file + 14);

    /* 12 is BITMAPCOREHEADER, which has 16-bit dimensions and no masks. */
    if (dib_size < 40 || dib_size > size - 14) goto done;

    int32_t  width   = rd32s(file + 18);
    int32_t  height  = rd32s(file + 22);
    uint16_t bpp     = rd16(file + 28);
    uint32_t compr   = rd32(file + 30);

    if (width <= 0 || height == 0) goto done;
    if (bpp != 24 && bpp != 32) goto done;
    if (compr != BI_RGB && compr != BI_BITFIELDS) goto done;

    /* A negative height means the rows are already top-down. */
    int top_down = height < 0;
    uint32_t rows = (uint32_t)(top_down ? -height : height);
    uint32_t cols = (uint32_t)width;

    /* Channel masks. BI_BITFIELDS puts them right after a 40-byte header;
     * V4 and V5 headers carry them inline at the same place. BI_RGB has
     * none, and means BGR(A) byte order. */
    uint32_t rmask, gmask, bmask, amask;
    if (compr == BI_BITFIELDS || dib_size >= 56) {
        if (14 + 52 > size) goto done;
        rmask = rd32(file + 14 + 40);
        gmask = rd32(file + 14 + 44);
        bmask = rd32(file + 14 + 48);
        amask = (dib_size >= 56) ? rd32(file + 14 + 52) : 0;
    } else {
        rmask = 0x00FF0000u;
        gmask = 0x0000FF00u;
        bmask = 0x000000FFu;
        amask = (bpp == 32) ? 0xFF000000u : 0;
    }
    if (!rmask || !gmask || !bmask) goto done;

    int rs = mask_shift(rmask), rw = mask_width(rmask);
    int gs = mask_shift(gmask), gw = mask_width(gmask);
    int bs = mask_shift(bmask), bw = mask_width(bmask);
    int as = mask_shift(amask), aw = mask_width(amask);

    /* Rows are padded out to a 4-byte boundary. */
    uint32_t bytes_pp  = bpp / 8;
    uint32_t row_bytes = ((cols * bytes_pp + 3u) / 4u) * 4u;

    if (data_off > size) goto done;
    if (row_bytes != 0 && rows > (size - data_off) / row_bytes) goto done;

    pixels = (uint32_t *)malloc((size_t)cols * rows * sizeof(uint32_t));
    if (!pixels) goto done;

    int any_alpha = 0;

    for (uint32_t y = 0; y < rows; y++) {
        /* Bottom-up files store the last row first. */
        uint32_t src_row = top_down ? y : (rows - 1u - y);
        const uint8_t *src = file + data_off + (size_t)src_row * row_bytes;
        uint32_t *dst = pixels + (size_t)y * cols;

        for (uint32_t x = 0; x < cols; x++) {
            const uint8_t *p = src + (size_t)x * bytes_pp;
            uint32_t raw = (bpp == 32)
                ? rd32(p)
                : ((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16));

            uint32_t r = chan(raw, rmask, rs, rw);
            uint32_t g = chan(raw, gmask, gs, gw);
            uint32_t b = chan(raw, bmask, bs, bw);
            uint32_t a = amask ? chan(raw, amask, as, aw) : 255u;

            if (a) any_alpha = 1;
            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    /* An all-zero alpha channel means the author never set one, not that
     * the whole image is invisible. Promote it to opaque. */
    if (amask && !any_alpha) {
        for (uint32_t i = 0; i < cols * rows; i++)
            pixels[i] |= 0xFF000000u;
    }

    out->width  = (int)cols;
    out->height = (int)rows;
    out->pixels = pixels;
    pixels = 0;
    rc = 0;

done:
    if (pixels) free(pixels);
    free(file);
    return rc;
}

void bmp_free(struct bmp_image *img) {
    if (!img) return;
    if (img->pixels) free(img->pixels);
    img->pixels = 0;
    img->width = 0;
    img->height = 0;
}
