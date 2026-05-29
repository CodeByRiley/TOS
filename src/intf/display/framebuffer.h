#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

int      framebuffer_init(uint64_t mb2_addr);
void     framebuffer_clear(uint32_t color);
void     framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
uint32_t *framebuffer_buffer(void);         // for DOOM/games to write directly
uint64_t framebuffer_phys(void);
uint32_t framebuffer_pitch(void);

/* Scatter-gather page access for sys_fb_map. In MB2 (bootstrap) mode the
 * pages are a contiguous run from the GRUB framebuffer base; in virtio-gpu
 * mode they may be arbitrarily scattered. Callers should iterate
 * [0, framebuffer_num_pages()) and map each one individually. */
uint32_t framebuffer_num_pages(void);
uint64_t framebuffer_phys_for_page(uint32_t idx);

/* Hand the framebuffer over to virtio-gpu. Allocates a fresh scatter-gather
 * pixel buffer sized to the current scanout, attaches it as the host
 * resource, and switches the active buffer mappings. Must be called after
 * virtio_gpu_init succeeds. Returns 0 on success; on failure the MB2-backed
 * buffer remains active. */
int      framebuffer_attach_virtio(void);

/* Poll virtio-gpu for a host-side window resize event. If one is pending,
 * tear down + recreate the scanout backing at the new size and return 1.
 * Returns 0 if nothing changed. Safe to call from the kernel tty thread. */
int      framebuffer_check_resize(void);

/* Push the back-buffer to the host scanout. No-op in MB2 mode (the
 * framebuffer IS the scanout). Called by the tty render loop after every
 * frame in virtio mode. Only the accumulated damage rect (set by
 * framebuffer_mark_damage) is transferred; if no damage has been
 * registered since the last present, this is a no-op. */
void     framebuffer_present(void);

/* Coalesce (x,y,w,h) into the pending damage rect. Anything that writes to
 * the kernel-side pixel buffer should call this so the next present
 * transfers only the touched region — otherwise the host sees stale
 * pixels. Clipped to the framebuffer. */
void     framebuffer_mark_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Dedicated kthread entry that polls + presents at PIT tick rate. Spawn once
 * after framebuffer_attach_virtio succeeds; runs forever. Decouples the
 * (synchronous virtio) flush from the tty render path so a slow virtio ACK
 * can't stall whoever marked damage. */
void     framebuffer_flush_thread_entry(void);

#endif
