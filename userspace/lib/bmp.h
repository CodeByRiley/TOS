/* userspace/lib/bmp.h , minimal BMP decoder.
 *
 * Decodes uncompressed 24- and 32-bit Windows BMPs into a top-down buffer
 * of 0xAARRGGBB pixels, which is the framebuffer's own layout plus an
 * alpha byte. Enough for cursors, titlebar glyphs and desktop icons; not
 * a general image library , no RLE, no palettes, no 16bpp.
 *
 * Implementation: userspace/lib/bmp.c.
 */
#ifndef BMP_H
#define BMP_H

#include <stdint.h>

struct bmp_image {
    int       width;
    int       height;
    uint32_t *pixels;   /* width*height, top-down, 0xAARRGGBB */
};

/* Load and decode `path`. Returns 0 on success, -1 on anything else:
 * missing file, unsupported layout, allocation failure. `out` is only
 * written on success, so a failed load leaves a caller's fallback intact.
 *
 * 24-bit images come back fully opaque. 32-bit images whose alpha channel
 * is entirely zero are also treated as opaque , plenty of tools write
 * that for images with no transparency, and honouring it literally would
 * make every such image invisible. */
int  bmp_load(const char *path, struct bmp_image *out);

/* Release the pixel buffer and zero the struct. Safe on a zeroed struct. */
void bmp_free(struct bmp_image *img);

#endif
