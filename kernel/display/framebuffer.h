/* kernel/display/framebuffer.h — framebuffer abstraction.
 *
 * Two backends sit behind this surface:
 *   - MB2 mode (boot default): a contiguous run of physical pages handed
 *     to us by GRUB. The kernel writes straight to the scanout.
 *   - virtio-gpu mode: a scatter-gather pixel buffer attached to the host
 *     resource. Damage tracking + framebuffer_present() flush touched
 *     rects across the virtqueue.
 *
 * Switching backends is one-way (MB2 → virtio); callers don't care which
 * is active beyond using framebuffer_present + framebuffer_mark_damage.
 *
 * Implementation: kernel/display/framebuffer.c.
 */
#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

#define FB_PRESENT_MAX_RECTS 16

/* Shared with userspace's syscall ABI. Coordinates and extents are pixels. */
struct fb_rect {
    uint32_t x, y, w, h;
};

_Static_assert(sizeof(struct fb_rect) == 16,
               "fb_rect must match the userspace ABI");

/* Probe MB2 tag 8 and prep the contiguous-page backend. */
int       framebuffer_init(uint64_t mb2_addr);

struct gfx_surface framebuffer_get_gfx_surface(void);

/* Both mark their own damage; callers do not need a separate mark call. */
void      framebuffer_clear(uint32_t color);
void      framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color);

uint32_t  framebuffer_width(void);
uint32_t  framebuffer_height(void);

/* Direct buffer pointer (used by DOOM and gfx). Writes here still need a
 * framebuffer_mark_damage() to actually present in virtio mode. */
uint32_t *framebuffer_buffer(void);
uint64_t  framebuffer_phys(void);
uint32_t  framebuffer_pitch(void);

/* Scatter-gather page accessors used by sys_fb_map. In MB2 mode the pages
 * form a contiguous run from the GRUB base; in virtio-gpu mode they may
 * be arbitrarily scattered. Iterate [0, num_pages) and map each. */
uint32_t  framebuffer_num_pages(void);
uint64_t  framebuffer_phys_for_page(uint32_t idx);

/* One-shot switch to the virtio-gpu backend. Allocates a reusable 64 MiB
 * backing pool and an oversized host resource so ordinary resizes only change
 * the scanout rectangle. Must be called after virtio_gpu_init succeeds. */
int       framebuffer_attach_virtio(void);

/* Poll virtio-gpu for a host-side window resize. Returns 1 if rebound
 * to a new size, 0 otherwise. Owned by the framebuffer flush thread. */
int       framebuffer_check_resize(void);

/* Increments after a successful virtio framebuffer resize. Consumers that
 * cache screen dimensions can poll this without submitting GPU commands. */
uint32_t  framebuffer_resize_generation(void);

/* Push accumulated damage to the host scanout. No-op in MB2 mode (the
 * framebuffer IS the scanout). Called by the tty render loop every
 * frame in virtio mode; only the dirty rect is transferred. */
void      framebuffer_present(void);

/* Add (x,y,w,h) to the pending damage rect. Anything that writes pixels
 * should call this so the next present transfers the touched region.
 * Clipped to framebuffer bounds. */
void      framebuffer_mark_damage(uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h);

/* Copy dirty rectangles from a validated userspace backbuffer. Large copies
 * are split into scanline lanes and dispatched to the AP work queue; CPU 0
 * processes one lane and supplies the single-core fallback. This call is
 * synchronous, so the caller may modify or release the source after return. */
int       framebuffer_present_user(uint64_t *user_pml4, int owner_pid,
                                   uint64_t source, uint32_t source_pitch,
                                   const struct fb_rect *rects,
                                   uint32_t rect_count);

/* Pin a compositor backbuffer logically by snapshotting its physical pages.
 * The VM layer rejects munmap over this range until unregister or owner exit. */
int       framebuffer_register_user(uint64_t *user_pml4, int owner_pid,
                                    uint64_t source, uint32_t source_pitch,
                                    uint64_t source_bytes);
int       framebuffer_unregister_user(uint64_t *user_pml4, int owner_pid);
int       framebuffer_user_buffer_registered(uint64_t *user_pml4,
                                              int owner_pid, uint64_t source,
                                              uint32_t source_pitch,
                                              uint64_t source_bytes);
int       framebuffer_registered_range_overlaps(uint64_t *user_pml4,
                                                 uint64_t base,
                                                 uint64_t bytes);

/* Kthread entry that polls + presents at PIT tick rate. Spawn once after
 * virtio attach; runs forever. Decouples the synchronous virtio ACK
 * from any caller that marked damage. */
void      framebuffer_flush_thread_entry(void);

#endif
