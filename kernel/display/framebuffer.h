/* kernel/display/framebuffer.h , framebuffer abstraction.
 *
 * One surface, and whichever backend currently owns the scanout behind it.
 * Which backend that is , firmware, virtio-gpu, VMSVGA , is deliberately not
 * part of this interface: callers draw, mark damage, and present. The set of
 * backends and the order they are probed in live in framebuffer.c.
 *
 * Call display_init() once at boot. After it returns, framebuffer_needs_flush
 * says whether the active backend wants the periodic present worker; nothing
 * else here varies by backend.
 *
 * Implementation: kernel/display/framebuffer.c.
 */
#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <utilities/types.h>
#include <arch/syscall_abi.h>

/* Probe MB2 tag 8 and prep the contiguous-page backend. */
int framebuffer_init(u64 mb2_addr);

struct gfx_surface framebuffer_get_gfx_surface(void);

/* Both mark their own damage; callers do not need a separate mark call. */
void framebuffer_clear(u32 color);
void framebuffer_putpixel(u32 x, u32 y, u32 color);

u32 framebuffer_width(void);
u32 framebuffer_height(void);

/* Direct buffer pointer (used by DOOM and gfx). Writes here still need a
 * framebuffer_mark_damage() to actually present in virtio mode. */
u32 *framebuffer_buffer(void);
u64 framebuffer_phys(void);
u32 framebuffer_pitch(void);

/* Scatter-gather page accessors used by sys_fb_map. In MB2 mode the pages
 * form a contiguous run from the GRUB base; in virtio-gpu mode they may
 * be arbitrarily scattered. Iterate [0, num_pages) and map each. */
u32 framebuffer_num_pages(void);
u64 framebuffer_phys_for_page(u32 idx);

/* Bring up the display: probe the firmware framebuffer, then take the best
 * backend available. Returns 0 once something is driving the scanout, -1 only
 * when there is no framebuffer at all. Callers do not learn which backend
 * won; ask framebuffer_needs_flush() whether a flush worker is wanted. */
int display_init(u64 mb2_addr);

/* Poll the active accelerated backend for a host-side window resize. Returns
 * 1 if rebound to a new size, 0 otherwise. Owned by the flush thread. */
int framebuffer_check_resize(void);

/* Increments after a successful accelerated framebuffer resize. Consumers
 * that cache screen dimensions can poll without submitting GPU commands. */
u32 framebuffer_resize_generation(void);

/* Push accumulated damage to the host scanout. No-op only for direct MB2;
 * shadow MB2 converts pixels and virtio transfers dirty rectangles. */
void framebuffer_present(void);

/* Whether the active backend needs the periodic present worker. */
int framebuffer_needs_flush(void);

/* Add (x,y,w,h) to the pending damage rect. Anything that writes pixels
 * should call this so the next present transfers the touched region.
 * Clipped to framebuffer bounds. */
void framebuffer_mark_damage(u32 x, u32 y, u32 w, u32 h);

/* Force the damage and scanout locks unlocked so the panic renderer can
 * draw. A fault can land anywhere , including inside framebuffer_present
 * with scanout_lock already held on this very CPU , and the panic screen
 * then deadlocks against the code it interrupted. Stealing the locks is
 * safe only because the caller is on its way to panic_halt(): no other
 * thread will ever observe the framebuffer again. Never call this from
 * anywhere but the panic path. */
void framebuffer_panic_takeover(void);

/* Copy dirty rectangles from a validated userspace backbuffer. Large copies
 * are split into scanline lanes and dispatched to the AP work queue; CPU 0
 * processes one lane and supplies the single-core fallback. This call is
 * synchronous, so the caller may modify or release the source after return. */
int framebuffer_present_user(u64 *user_pml4, int owner_pid, u64 source,
                             u32 source_pitch, const struct fb_rect *rects,
                             u32 rect_count);

/* Pin a compositor backbuffer logically by snapshotting its physical pages.
 * The VM layer rejects munmap over this range until unregister or owner exit.
 */
int framebuffer_register_user(u64 *user_pml4, int owner_pid, u64 source,
                              u32 source_pitch, u64 source_bytes);
int framebuffer_unregister_user(u64 *user_pml4, int owner_pid);
int framebuffer_user_buffer_registered(u64 *user_pml4, int owner_pid,
                                       u64 source, u32 source_pitch,
                                       u64 source_bytes);
int framebuffer_registered_range_overlaps(u64 *user_pml4, u64 base, u64 bytes);

/* Kthread entry that polls + presents at PIT tick rate. Spawn once for any
 * backend needing explicit flushes; runs forever. */
void framebuffer_flush_thread_entry(void);

#endif
