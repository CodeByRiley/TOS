#include "display/framebuffer.h"
#include "boot/multiboot2.h"
#include "memory/vmm.h"
#include "memory/pmm.h"
#include "utilities/log.h"
#include <stdint.h>

#define FB_VIRT_BASE 0xFFFFE00000000000ULL

static uint32_t *fb       = 0;
static uint32_t  fb_w     = 0;
static uint32_t  fb_h     = 0;
static uint32_t  fb_pitch = 0;       // bytes per row

int framebuffer_init(uint64_t mb2_addr) {
    struct MB2_TAG_FRAMEBUFFER *t =
        (struct MB2_TAG_FRAMEBUFFER*)mb2_find_tag(mb2_addr, MULTIBOOT_TAG_FRAMEBUFFER);
    if (!t) {
        log_write("DISPLAY: no framebuffer tag", KERNEL, LOG_ERROR);
        return -1;
    }
    if (t->fb_type != FB_TYPE_RGB) {
        log_write_hex("FB: unsupported type =", t->fb_type, KERNEL, LOG_ERROR);
        return -1;
    }
    if (t->bpp != 24 && t->bpp != 32) {
        log_write_hex("FB: unsupported bpp =", t->bpp, KERNEL, LOG_ERROR);
        return -1;
    }

    // log RGB layout for debugging
    log_write_hex("FB: red_pos   =", t->red_pos,   KERNEL, LOG_INFO);
    log_write_hex("FB: green_pos =", t->green_pos, KERNEL, LOG_INFO);
    log_write_hex("FB: blue_pos  =", t->blue_pos,  KERNEL, LOG_INFO);

    fb_w     = t->width;
    fb_h     = t->height;
    fb_pitch = t->pitch;

    // map physical framebuffer into virtual space
    uint64_t fb_bytes = (uint64_t)t->pitch * t->height;
    uint64_t pages    = (fb_bytes + 4095) / 4096;
    uint64_t phys     = t->addr;

    for (uint64_t i = 0; i < pages; i++) {
        vmm_map(FB_VIRT_BASE + i * 4096,
                phys + i * 4096,
                VMM_PRESENT | VMM_WRITE);
    }
    fb = (uint32_t*)FB_VIRT_BASE;

    log_write_hex("DISPLAY: phys      =", t->addr,  KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: width     =", fb_w,     KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: height    =", fb_h,     KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: pitch     =", fb_pitch, KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: bpp       =", t->bpp,   KERNEL, LOG_INFO);
    return 0;
}

void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_w || y >= fb_h) return;
    uint32_t *row = (uint32_t*)((uint8_t*)fb + y * fb_pitch);
    row[x] = color;
}

void framebuffer_clear(uint32_t color) {
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t *row = (uint32_t*)((uint8_t*)fb + y * fb_pitch);
        for (uint32_t x = 0; x < fb_w; x++) row[x] = color;
    }
}

uint32_t  framebuffer_width(void)  { return fb_w; }
uint32_t  framebuffer_height(void) { return fb_h; }
uint32_t *framebuffer_buffer(void) { return fb; }
