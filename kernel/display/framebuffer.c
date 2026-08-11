/* kernel/display/framebuffer.c — framebuffer abstraction.
 *
 * Two backends behind a single API:
 *   MB2 mode    — the contiguous run of physical pages GRUB handed us.
 *                  Direct writes are visible on the scanout.
 *   virtio-gpu  — a scatter-gather pixel buffer attached to the host
 *                  resource. Damage tracking + framebuffer_present()
 *                  flush touched rects over the virtqueue.
 *
 * Page tables map either backend's pages contiguously at FB_VIRT_BASE so
 * pixel writers don't need to care which mode is active. Switching is
 * one-way (MB2 → virtio); on virtio failure the MB2 buffer stays live.
 *
 * The flush thread (framebuffer_flush_thread_entry) polls + presents at
 * PIT tick rate so the (synchronous virtio ACK) flush is decoupled from
 * whoever marked damage.
 */
#include "display/framebuffer.h"
#include "boot/multiboot2.h"
#include "input/mouse.h"
#include "memory/vmm.h"
#include "memory/pmm.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include "virtio/virtio_gpu.h"
#include "sched/sched.h"
#include <stdint.h>

#define FB_VIRT_BASE  0xFFFFE00000000000ULL
/* Upper bound on FB size — 64 MiB. Covers 4K@32bpp (33 MiB) with headroom.
 * Anything bigger fails attach_virtio cleanly rather than corrupting state. */
#define FB_MAX_PAGES  (64ULL * 1024 * 1024 / 4096)

#define FB_MODE_NONE   0
#define FB_MODE_MB2    1
#define FB_MODE_VIRTIO 2

static uint32_t *fb       = 0;
static uint32_t  fb_w     = 0;
static uint32_t  fb_h     = 0;
static uint32_t  fb_pitch = 0;       /* bytes per row */
static uint32_t  fb_mode  = FB_MODE_NONE;

/* Damage rect: pending region to push to the host scanout on next present.
 * Coalesces by bounding box — cheap, slightly over-flushes when touches are
 * scattered. dirty=0 means "nothing to push" and present is a no-op. */
static uint32_t  dmg_x = 0, dmg_y = 0, dmg_w = 0, dmg_h = 0;
static int       dmg_dirty = 0;

void framebuffer_mark_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (fb_w == 0 || fb_h == 0) return;
    if (x >= fb_w || y >= fb_h) return;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w == 0 || h == 0) return;
    if (!dmg_dirty) {
        dmg_x = x; dmg_y = y; dmg_w = w; dmg_h = h; dmg_dirty = 1;
        return;
    }
    uint32_t x0 = dmg_x < x ? dmg_x : x;
    uint32_t y0 = dmg_y < y ? dmg_y : y;
    uint32_t x1a = dmg_x + dmg_w, x1b = x + w;
    uint32_t y1a = dmg_y + dmg_h, y1b = y + h;
    uint32_t x1 = x1a > x1b ? x1a : x1b;
    uint32_t y1 = y1a > y1b ? y1a : y1b;
    dmg_x = x0; dmg_y = y0; dmg_w = x1 - x0; dmg_h = y1 - y0;
}

static void mark_full_damage(void) {
    dmg_x = 0; dmg_y = 0; dmg_w = fb_w; dmg_h = fb_h; dmg_dirty = 1;
}

/* Backing page list. In MB2 mode this is a synthetic enumeration of the
 * contiguous MMIO range. In virtio mode it's the scattered PMM frames
 * handed to RESOURCE_ATTACH_BACKING. */
static uint64_t  fb_pages[FB_MAX_PAGES];
static uint32_t  fb_n_pages = 0;
static uint64_t  fb_mb2_phys = 0;   /* preserved so phys() still answers in MB2 mode */

uint64_t framebuffer_phys(void) {
    if (fb_mode == FB_MODE_MB2)    return fb_mb2_phys;
    if (fb_mode == FB_MODE_VIRTIO && fb_n_pages > 0) return fb_pages[0];
    return 0;
}
uint32_t framebuffer_pitch(void) { return fb_pitch; }
uint32_t framebuffer_width(void)  { return fb_w; }
uint32_t framebuffer_height(void) { return fb_h; }
uint32_t *framebuffer_buffer(void) { return fb; }
uint32_t framebuffer_num_pages(void) { return fb_n_pages; }
uint64_t framebuffer_phys_for_page(uint32_t idx) {
    if (idx >= fb_n_pages) return 0;
    return fb_pages[idx];
}

static int map_pages_at_vbase(const uint64_t *pages, uint32_t n, int cacheable) {
    uint64_t flags = VMM_PRESENT | VMM_WRITE;
    if (!cacheable) flags |= VMM_PCD | VMM_PWT;
    for (uint32_t i = 0; i < n; i++) {
        if (vmm_map(FB_VIRT_BASE + (uint64_t)i * 4096, pages[i], flags) != 0) {
            return -1;
        }
    }
    return 0;
}

static void unmap_pages_at_vbase(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        vmm_unmap(FB_VIRT_BASE + (uint64_t)i * 4096);
    }
}

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

    log_write_hex("FB: red_pos   =", t->red_pos,   KERNEL, LOG_INFO);
    log_write_hex("FB: green_pos =", t->green_pos, KERNEL, LOG_INFO);
    log_write_hex("FB: blue_pos  =", t->blue_pos,  KERNEL, LOG_INFO);

    fb_w        = t->width;
    fb_h        = t->height;
    fb_pitch    = t->pitch;
    fb_mb2_phys = t->addr;

    uint64_t fb_bytes = (uint64_t)t->pitch * t->height;
    uint64_t pages    = (fb_bytes + 4095) / 4096;
    if (pages > FB_MAX_PAGES) {
        log_write_hex("FB: MB2 buffer too large, pages =", pages, KERNEL, LOG_ERROR);
        return -1;
    }
    /* Synthesise a page list from the contiguous MMIO base so that sys_fb_map
     * (and the eventual switch to virtio) can iterate without caring which
     * mode we're in. */
    for (uint32_t i = 0; i < pages; i++) {
        fb_pages[i] = t->addr + (uint64_t)i * 4096;
    }
    fb_n_pages = (uint32_t)pages;

    if (map_pages_at_vbase(fb_pages, fb_n_pages, /*cacheable=*/0) != 0) {
        log_write("FB: MB2 map failed", KERNEL, LOG_ERROR);
        return -1;
    }
    fb = (uint32_t*)FB_VIRT_BASE;
    fb_mode = FB_MODE_MB2;
    mark_full_damage();

    log_write_hex("DISPLAY: phys      =", t->addr,  KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: width     =", fb_w,     KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: height    =", fb_h,     KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: pitch     =", fb_pitch, KERNEL, LOG_INFO);
    log_write_hex("DISPLAY: bpp       =", t->bpp,   KERNEL, LOG_INFO);
    return 0;
}

/* Tear down the current backing: unmap pages from FB_VIRT_BASE, and in virtio
 * mode also free the underlying PMM frames. MB2 frames are MMIO, not owned by
 * pmm, so leave them alone. */
static void teardown_current(void) {
    if (fb_n_pages == 0) return;
    unmap_pages_at_vbase(fb_n_pages);
    if (fb_mode == FB_MODE_VIRTIO) {
        for (uint32_t i = 0; i < fb_n_pages; i++) pmm_free_frame(fb_pages[i]);
    }
    fb_n_pages = 0;
}

/* Allocate `n_pages` of pmm-backed pixel buffer, populate fb_pages, map at
 * FB_VIRT_BASE (cacheable), and zero the new buffer. Caller has already
 * cleared the previous mapping. Returns 0 on success. */
static int alloc_and_map_virtio(uint32_t n_pages) {
    if (n_pages > FB_MAX_PAGES) {
        log_write_hex("FB: virtio backing too large, pages =", n_pages, KERNEL, LOG_ERROR);
        return -1;
    }
    for (uint32_t i = 0; i < n_pages; i++) {
        uint64_t p = pmm_alloc_frame();
        if (!p) {
            log_write("FB: pmm exhausted during attach", KERNEL, LOG_ERROR);
            for (uint32_t j = 0; j < i; j++) pmm_free_frame(fb_pages[j]);
            return -1;
        }
        fb_pages[i] = p;
    }
    fb_n_pages = n_pages;
    if (map_pages_at_vbase(fb_pages, fb_n_pages, /*cacheable=*/1) != 0) {
        log_write("FB: virtio map failed", KERNEL, LOG_ERROR);
        for (uint32_t i = 0; i < n_pages; i++) pmm_free_frame(fb_pages[i]);
        fb_n_pages = 0;
        return -1;
    }
    memset((void*)FB_VIRT_BASE, 0, (uint64_t)n_pages * 4096);
    return 0;
}

int framebuffer_attach_virtio(void) {
    uint32_t w = 0, h = 0;
    if (virtio_gpu_get_dims(&w, &h) != 0) return -1;
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096);

    teardown_current();
    if (alloc_and_map_virtio(pages) != 0) return -1;

    if (virtio_gpu_set_scanout_2d(w, h, fb_pages, fb_n_pages) != 0) {
        log_write("FB: virtio set_scanout failed", KERNEL, LOG_ERROR);
        return -1;
    }

    fb       = (uint32_t*)FB_VIRT_BASE;
    fb_w     = w;
    fb_h     = h;
    fb_pitch = w * 4;
    fb_mode  = FB_MODE_VIRTIO;
    mark_full_damage();
    log_write_hex("FB: virtio attached, w =", fb_w, KERNEL, LOG_INFO);
    log_write_hex("FB: virtio attached, h =", fb_h, KERNEL, LOG_INFO);
    return 0;
}

static int do_resize(uint32_t w, uint32_t h) {
    if (fb_mode != FB_MODE_VIRTIO) return -1;
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096);

    teardown_current();
    if (alloc_and_map_virtio(pages) != 0) return -1;
    if (virtio_gpu_set_scanout_2d(w, h, fb_pages, fb_n_pages) != 0) {
        log_write("FB: virtio resize set_scanout failed", KERNEL, LOG_ERROR);
        return -1;
    }
    fb_w     = w;
    fb_h     = h;
    fb_pitch = w * 4;
    fb       = (uint32_t*)FB_VIRT_BASE;
    mouse_set_bounds(fb_w, fb_h);
    mark_full_damage();
    log_write_hex("FB: resized, w =", fb_w, KERNEL, LOG_INFO);
    log_write_hex("FB: resized, h =", fb_h, KERNEL, LOG_INFO);
    return 0;
}

int framebuffer_check_resize(void) {
    if (fb_mode != FB_MODE_VIRTIO) return 0;
    if (!virtio_gpu_poll_display_event()) return 0;

    uint32_t w = 0, h = 0;
    if (virtio_gpu_get_dims(&w, &h) != 0) return 0;
    if (w == fb_w && h == fb_h) return 0;

    if (do_resize(w, h) != 0) return 0;
    return 1;
}

void framebuffer_present(void) {
    if (fb_mode != FB_MODE_VIRTIO) return;
    if (!dmg_dirty) return;
    /* Snapshot + clear before the (synchronous) flush so any writes that
     * happen concurrently re-mark damage cleanly. Producers only write
     * dmg_*; consumer only reads then clears. Single-CPU kthread + no
     * preemption mid-instruction = no torn reads here. */
    uint32_t x = dmg_x, y = dmg_y, w = dmg_w, h = dmg_h;
    dmg_dirty = 0;
    dmg_x = dmg_y = dmg_w = dmg_h = 0;
    virtio_gpu_flush_rect(x, y, w, h);
}

void framebuffer_flush_thread_entry(void) {
    for (;;) {
        framebuffer_present();
        task_sleep_ticks(1);   /* 100 Hz upper bound on flush rate */
    }
}

/* The simplest possible "graphics" API: one pixel, one store, no batching,
 * no surface, no DMA. If anyone tries to build a real UI on top of putpixel,
 * we're going to see exactly the framerate it deserves. */
void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_w || y >= fb_h) return;
    uint32_t *row = (uint32_t*)((uint8_t*)fb + y * fb_pitch);
    row[x] = color;
    framebuffer_mark_damage(x, y, 1, 1);
}

void framebuffer_clear(uint32_t color) {
    /* Fast path when pitch is tight (no padding between rows): the whole
     * framebuffer is contiguous, do one long stosd-shaped sweep. The slow
     * path still walks rows so we don't trample padding bytes some
     * pre-2010s GPU decided it cared about. */
    mark_full_damage();
    if (fb_pitch == fb_w * 4) {
        uint32_t *p = fb;
        uint32_t  n = fb_w * fb_h;
        for (uint32_t i = 0; i < n; i++) p[i] = color;
        return;
    }
    /* Strength-reduced: advance row pointer by pitch instead of multiplying
     * y*pitch every row. */
    uint8_t *p = (uint8_t*)fb;
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t *row = (uint32_t*)p;
        for (uint32_t x = 0; x < fb_w; x++) row[x] = color;
        p += fb_pitch;
    }
}
