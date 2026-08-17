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
#include <boot/multiboot2.h>
#include <display/framebuffer.h>
#include <display/graphics.h>
#include <drivers/video/virtio/virtio_gpu.h>
#include <input/mouse.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <sched/sched.h>
#include <sched/smp.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <utilities/log.h>
#include <utilities/string.h>

#define FB_VIRT_BASE 0xFFFFE00000000000ULL
/* Upper bound on FB size — 64 MiB. Covers 4K@32bpp (33 MiB) with headroom.
 * Anything bigger fails attach_virtio cleanly rather than corrupting state. */
#define FB_MAX_BYTES (64ULL * 1024 * 1024)
#define FB_MAX_PAGES (FB_MAX_BYTES / 4096)
#define FB_MIN_RESOURCE_W 2048U
#define FB_MIN_RESOURCE_H 2048U

#define FB_MODE_NONE 0
#define FB_MODE_MB2 1
#define FB_MODE_VIRTIO 2

#define FB_MAX_DAMAGE 8
#define FB_DAMAGE_MERGE_SLACK 8192

struct fb_damage_rect {
  uint32_t x, y, w, h;
};

static uint32_t *fb = 0;
static uint32_t fb_w = 0;
static uint32_t fb_h = 0;
static uint32_t fb_pitch = 0; /* bytes per row */
static uint32_t fb_mode = FB_MODE_NONE;
static uint32_t fb_resize_generation = 0;

/* Damage rects: pending regions to push to the host scanout on next present.
 * A small fixed set keeps thin, distant writes (cursor + drag ghost strips)
 * from being flattened into one huge bounding box. Adjacent/overlapping
 * damage still merges so normal drawing does not burn a slot per pixel. */
static struct fb_damage_rect dmg[FB_MAX_DAMAGE];
static int dmg_count = 0;
static spinlock_t damage_lock = SPINLOCK_INIT;
/* Serializes writes to the guest backing store against VirtIO transfers.
 * Userspace overlays must enter through framebuffer_present_user to take it. */
static spinlock_t scanout_lock = SPINLOCK_INIT;

static uint64_t damage_area(const struct fb_damage_rect *r) {
  return (uint64_t)r->w * (uint64_t)r->h;
}

static void damage_union(struct fb_damage_rect *out,
                         const struct fb_damage_rect *a,
                         const struct fb_damage_rect *b) {
  uint32_t x0 = a->x < b->x ? a->x : b->x;
  uint32_t y0 = a->y < b->y ? a->y : b->y;
  uint32_t x1a = a->x + a->w, x1b = b->x + b->w;
  uint32_t y1a = a->y + a->h, y1b = b->y + b->h;
  uint32_t x1 = x1a > x1b ? x1a : x1b;
  uint32_t y1 = y1a > y1b ? y1a : y1b;
  out->x = x0;
  out->y = y0;
  out->w = x1 - x0;
  out->h = y1 - y0;
}

static uint64_t damage_merge_waste(const struct fb_damage_rect *a,
                                   const struct fb_damage_rect *b) {
  struct fb_damage_rect u;
  damage_union(&u, a, b);
  uint64_t sum = damage_area(a) + damage_area(b);
  uint64_t area = damage_area(&u);
  return area > sum ? area - sum : 0;
}

void framebuffer_mark_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  spin_lock(&damage_lock);
  if (fb_w == 0 || fb_h == 0 || x >= fb_w || y >= fb_h) {
    spin_unlock(&damage_lock);
    return;
  }
  if (w > fb_w - x)
    w = fb_w - x;
  if (h > fb_h - y)
    h = fb_h - y;
  if (w == 0 || h == 0) {
    spin_unlock(&damage_lock);
    return;
  }

  struct fb_damage_rect r = {x, y, w, h};

  int best = -1;
  uint64_t best_waste = 0;
  for (int i = 0; i < dmg_count; i++) {
    uint64_t waste = damage_merge_waste(&dmg[i], &r);
    if (waste > FB_DAMAGE_MERGE_SLACK)
      continue;
    if (best < 0 || waste < best_waste) {
      best = i;
      best_waste = waste;
    }
  }
  if (best >= 0) {
    damage_union(&dmg[best], &dmg[best], &r);
    spin_unlock(&damage_lock);
    return;
  }

  if (dmg_count < FB_MAX_DAMAGE) {
    dmg[dmg_count++] = r;
    spin_unlock(&damage_lock);
    return;
  }

  int ai = 0, bi = 1;
  uint64_t least = damage_merge_waste(&dmg[0], &dmg[1]);
  for (int i = 0; i < dmg_count; i++) {
    for (int j = i + 1; j < dmg_count; j++) {
      uint64_t waste = damage_merge_waste(&dmg[i], &dmg[j]);
      if (waste < least) {
        least = waste;
        ai = i;
        bi = j;
      }
    }
  }
  damage_union(&dmg[ai], &dmg[ai], &dmg[bi]);
  dmg[bi] = dmg[dmg_count - 1];
  dmg[dmg_count - 1] = r;
  spin_unlock(&damage_lock);
}

static void mark_full_damage(void) {
  spin_lock(&damage_lock);
  if (fb_w == 0 || fb_h == 0) {
    dmg_count = 0;
    spin_unlock(&damage_lock);
    return;
  }
  dmg[0] = (struct fb_damage_rect){0, 0, fb_w, fb_h};
  dmg_count = 1;
  spin_unlock(&damage_lock);
}

/* Backing page list. In MB2 mode this is a synthetic enumeration of the
 * contiguous MMIO range. In virtio mode it's the scattered PMM frames
 * handed to RESOURCE_ATTACH_BACKING. */
static uint64_t fb_pages[FB_MAX_PAGES];
static uint32_t fb_n_pages = 0;      /* visible span exposed by sys_fb_map */
static uint32_t fb_mapped_pages = 0; /* prefix mapped at FB_VIRT_BASE */
static uint32_t fb_owned_pages = 0;  /* PMM frames owned by virtio mode */
static uint64_t fb_pool_phys = 0;    /* contiguous virtio backing base */
static uint32_t fb_resource_w = 0;
static uint32_t fb_resource_h = 0;
static uint64_t fb_mb2_phys =
    0; /* preserved so phys() still answers in MB2 mode */

uint64_t framebuffer_phys(void) {
  if (fb_mode == FB_MODE_MB2)
    return fb_mb2_phys;
  if (fb_mode == FB_MODE_VIRTIO && fb_n_pages > 0)
    return fb_pages[0];
  return 0;
}
uint32_t framebuffer_pitch(void) { return fb_pitch; }
uint32_t framebuffer_width(void) { return fb_w; }
uint32_t framebuffer_height(void) { return fb_h; }
uint32_t *framebuffer_buffer(void) { return fb; }
uint32_t framebuffer_num_pages(void) { return fb_n_pages; }
uint64_t framebuffer_phys_for_page(uint32_t idx) {
  if (idx >= fb_n_pages)
    return 0;
  return fb_pages[idx];
}

#define FB_COPY_MAX_LANES 8
#define FB_COPY_PARALLEL_MIN_BYTES (256ULL * 1024ULL)

struct fb_copy_batch {
  uint64_t *user_pml4;
  uint64_t source;
  uint32_t source_pitch;
  const uint64_t *source_pages;
  uint64_t source_page_base;
  uint32_t source_page_count;
  uint8_t *destination;
  uint32_t destination_pitch;
  struct fb_rect rects[FB_PRESENT_MAX_RECTS];
  uint32_t rect_count;
  uint32_t lane_count;
  volatile uint32_t remaining;
  volatile int failed;
};

struct fb_copy_job {
  struct fb_copy_batch *batch;
  uint32_t lane;
};

static volatile int fb_parallel_copy_logged;

struct fb_user_registration {
  uint64_t *user_pml4;
  int owner_pid;
  uint64_t source;
  uint64_t source_bytes;
  uint64_t page_base;
  uint32_t source_pitch;
  uint32_t page_count;
  uint64_t *pages;
};

static struct fb_user_registration user_registration;
static spinlock_t user_registration_lock = SPINLOCK_INIT;

static int registration_matches(const struct fb_user_registration *registration,
                                uint64_t *user_pml4, int owner_pid,
                                uint64_t source, uint32_t source_pitch,
                                uint64_t source_bytes) {
  return registration->pages && registration->user_pml4 == user_pml4 &&
         registration->owner_pid == owner_pid &&
         registration->source == source &&
         registration->source_pitch == source_pitch &&
         registration->source_bytes >= source_bytes;
}

int framebuffer_register_user(uint64_t *user_pml4, int owner_pid,
                              uint64_t source, uint32_t source_pitch,
                              uint64_t source_bytes) {
  if (!user_pml4 || owner_pid <= 0 || !source || source_bytes == 0 ||
      source + source_bytes < source) {
    return -1;
  }

  uint64_t page_base = source & ~4095ULL;
  uint64_t last_page = (source + source_bytes - 1) & ~4095ULL;
  uint64_t page_count64 = (last_page - page_base) / 4096ULL + 1;
  if (page_count64 == 0 || page_count64 > FB_MAX_PAGES)
    return -1;

  uint32_t page_count = (uint32_t)page_count64;
  uint64_t *pages = kmalloc((size_t)page_count * sizeof(*pages));
  if (!pages)
    return -1;

  for (uint32_t i = 0; i < page_count; i++) {
    uint64_t va = page_base + (uint64_t)i * 4096ULL;
    uint64_t entry = vmm_entry_in(user_pml4, va);
    if (!(entry & VMM_PRESENT) || !(entry & VMM_USER)) {
      kfree(pages);
      return -1;
    }
    pages[i] = entry & VMM_ADDR_MASK;
  }

  spin_lock(&user_registration_lock);
  uint64_t *old_pages = user_registration.pages;
  user_registration = (struct fb_user_registration){
      .user_pml4 = user_pml4,
      .owner_pid = owner_pid,
      .source = source,
      .source_bytes = source_bytes,
      .page_base = page_base,
      .source_pitch = source_pitch,
      .page_count = page_count,
      .pages = pages,
  };
  spin_unlock(&user_registration_lock);

  if (old_pages)
    kfree(old_pages);
  log_write_hex("FB: registered backbuffer pages =", page_count, KERNEL,
                LOG_INFO);
  return 0;
}

int framebuffer_unregister_user(uint64_t *user_pml4, int owner_pid) {
  spin_lock(&user_registration_lock);
  if (!user_registration.pages) {
    spin_unlock(&user_registration_lock);
    return 0;
  }
  if (user_registration.user_pml4 != user_pml4 ||
      user_registration.owner_pid != owner_pid) {
    spin_unlock(&user_registration_lock);
    return -1;
  }

  uint64_t *pages = user_registration.pages;
  user_registration = (struct fb_user_registration){0};
  spin_unlock(&user_registration_lock);
  kfree(pages);
  return 0;
}

int framebuffer_user_buffer_registered(uint64_t *user_pml4, int owner_pid,
                                       uint64_t source, uint32_t source_pitch,
                                       uint64_t source_bytes) {
  spin_lock(&user_registration_lock);
  int matched = registration_matches(&user_registration, user_pml4, owner_pid,
                                     source, source_pitch, source_bytes);
  spin_unlock(&user_registration_lock);
  return matched;
}

int framebuffer_registered_range_overlaps(uint64_t *user_pml4, uint64_t base,
                                          uint64_t bytes) {
  if (!user_pml4 || bytes == 0 || base + bytes < base)
    return 0;

  spin_lock(&user_registration_lock);
  int overlaps = 0;
  if (user_registration.pages && user_registration.user_pml4 == user_pml4) {
    uint64_t registered_end =
        user_registration.source + user_registration.source_bytes;
    uint64_t range_end = base + bytes;
    overlaps = base < registered_end && range_end > user_registration.source;
  }
  spin_unlock(&user_registration_lock);
  return overlaps;
}

/* APs do not run the caller's CR3. Registered buffers use a stable physical
 * page snapshot; unregistered callers fall back to walking the PML4 after the
 * syscall layer validates the source span. */
static int copy_user_pixels(const struct fb_copy_batch *batch, uint64_t source,
                            uint8_t *destination, uint32_t bytes) {
  while (bytes > 0) {
    uint64_t physical;
    if (batch->source_pages) {
      if (source < batch->source_page_base)
        return -1;
      uint64_t offset = source - batch->source_page_base;
      uint64_t page = offset / 4096ULL;
      if (page >= batch->source_page_count)
        return -1;
      physical = batch->source_pages[page] + (offset & 4095ULL);
    } else {
      uint64_t entry = vmm_entry_in(batch->user_pml4, source);
      if (!(entry & VMM_PRESENT) || !(entry & VMM_USER))
        return -1;
      physical = vmm_translate_in(batch->user_pml4, source);
      if (!physical)
        return -1;
    }

    uint32_t chunk = 4096U - (uint32_t)(source & 4095U);
    if (chunk > bytes)
      chunk = bytes;
    memcpy(destination, phys_to_virt(physical), chunk);
    destination += chunk;
    source += chunk;
    bytes -= chunk;
  }
  return 0;
}

static void framebuffer_copy_lane(void *argument) {
  struct fb_copy_job *job = (struct fb_copy_job *)argument;
  struct fb_copy_batch *batch = job->batch;

  for (uint32_t i = 0; i < batch->rect_count; i++) {
    const struct fb_rect *rect = &batch->rects[i];
    for (uint32_t row = job->lane; row < rect->h; row += batch->lane_count) {
      if (__atomic_load_n(&batch->failed, __ATOMIC_ACQUIRE))
        break;

      uint64_t source = batch->source +
                        (uint64_t)(rect->y + row) * batch->source_pitch +
                        (uint64_t)rect->x * 4ULL;
      uint8_t *destination =
          batch->destination +
          (uint64_t)(rect->y + row) * batch->destination_pitch +
          (uint64_t)rect->x * 4ULL;
      if (copy_user_pixels(batch, source, destination, rect->w * 4U) != 0) {
        __atomic_store_n(&batch->failed, 1, __ATOMIC_RELEASE);
        break;
      }
    }
  }

  __atomic_sub_fetch(&batch->remaining, 1, __ATOMIC_ACQ_REL);
}

int framebuffer_present_user(uint64_t *user_pml4, int owner_pid,
                             uint64_t source, uint32_t source_pitch,
                             const struct fb_rect *rects, uint32_t rect_count) {
  if (!user_pml4 || !source || !rects || rect_count == 0 ||
      rect_count > FB_PRESENT_MAX_RECTS || !fb || fb_w == 0 || fb_h == 0 ||
      source_pitch < fb_w * 4U) {
    return -1;
  }

  struct fb_copy_batch batch = {
      .user_pml4 = user_pml4,
      .source = source,
      .source_pitch = source_pitch,
      .destination = (uint8_t *)fb,
      .destination_pitch = fb_pitch,
  };

  uint64_t required_bytes =
      (uint64_t)source_pitch * (fb_h - 1) + (uint64_t)fb_w * 4ULL;
  int registration_locked = 0;
  spin_lock(&user_registration_lock);
  if (registration_matches(&user_registration, user_pml4, owner_pid, source,
                           source_pitch, required_bytes)) {
    batch.source_pages = user_registration.pages;
    batch.source_page_base = user_registration.page_base;
    batch.source_page_count = user_registration.page_count;
    registration_locked = 1;
  } else {
    spin_unlock(&user_registration_lock);
  }

  uint64_t total_bytes = 0;
  uint64_t total_rows = 0;
  for (uint32_t i = 0; i < rect_count; i++) {
    struct fb_rect rect = rects[i];
    if (rect.w == 0 || rect.h == 0 || rect.x >= fb_w || rect.y >= fb_h)
      continue;
    if (rect.w > fb_w - rect.x)
      rect.w = fb_w - rect.x;
    if (rect.h > fb_h - rect.y)
      rect.h = fb_h - rect.y;
    batch.rects[batch.rect_count++] = rect;
    total_bytes += (uint64_t)rect.w * 4ULL * rect.h;
    total_rows += rect.h;
  }
  if (batch.rect_count == 0) {
    if (registration_locked)
      spin_unlock(&user_registration_lock);
    return 0;
  }

  uint32_t lanes = 1;
  int ap_workers = smp_worker_count();
  if (ap_workers > 0 && total_bytes >= FB_COPY_PARALLEL_MIN_BYTES) {
    uint64_t wanted = (total_bytes + FB_COPY_PARALLEL_MIN_BYTES - 1) /
                      FB_COPY_PARALLEL_MIN_BYTES;
    uint64_t available = (uint64_t)ap_workers + 1;
    if (wanted > available)
      wanted = available;
    if (wanted > FB_COPY_MAX_LANES)
      wanted = FB_COPY_MAX_LANES;
    if (wanted > total_rows)
      wanted = total_rows;
    lanes = (uint32_t)wanted;
  }
  batch.lane_count = lanes;
  batch.remaining = lanes;

  spin_lock(&scanout_lock);

  if (lanes > 1 &&
      __atomic_exchange_n(&fb_parallel_copy_logged, 1, __ATOMIC_ACQ_REL) == 0) {
    log_write_hex("FB: parallel present lanes =", lanes, KERNEL, LOG_INFO);
  }

  struct fb_copy_job jobs[FB_COPY_MAX_LANES];
  jobs[0] = (struct fb_copy_job){.batch = &batch, .lane = 0};
  for (uint32_t lane = 1; lane < lanes; lane++) {
    jobs[lane] = (struct fb_copy_job){.batch = &batch, .lane = lane};
    if (smp_submit_work(framebuffer_copy_lane, &jobs[lane]) != 0)
      framebuffer_copy_lane(&jobs[lane]);
  }

  framebuffer_copy_lane(&jobs[0]);
  while (__atomic_load_n(&batch.remaining, __ATOMIC_ACQUIRE) != 0)
    __asm__ volatile("pause");

  spin_unlock(&scanout_lock);

  if (registration_locked)
    spin_unlock(&user_registration_lock);

  if (!__atomic_load_n(&batch.failed, __ATOMIC_ACQUIRE)) {
    for (uint32_t i = 0; i < batch.rect_count; i++) {
      const struct fb_rect *rect = &batch.rects[i];
      framebuffer_mark_damage(rect->x, rect->y, rect->w, rect->h);
    }
  }
  return __atomic_load_n(&batch.failed, __ATOMIC_ACQUIRE) ? -1 : 0;
}

static int map_pages_at_vbase(const uint64_t *pages, uint32_t n,
                              int cacheable) {
  uint64_t flags = VMM_PRESENT | VMM_WRITE;
  if (!cacheable)
    flags |= VMM_PCD | VMM_PWT;
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
  struct MB2_TAG_FRAMEBUFFER *t = (struct MB2_TAG_FRAMEBUFFER *)mb2_find_tag(
      mb2_addr, MULTIBOOT_TAG_FRAMEBUFFER);
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

  log_write_hex("FB: red_pos   =", t->red_pos, KERNEL, LOG_INFO);
  log_write_hex("FB: green_pos =", t->green_pos, KERNEL, LOG_INFO);
  log_write_hex("FB: blue_pos  =", t->blue_pos, KERNEL, LOG_INFO);

  fb_w = t->width;
  fb_h = t->height;
  fb_pitch = t->pitch;
  fb_mb2_phys = t->addr;

  uint64_t fb_bytes = (uint64_t)t->pitch * t->height;
  uint64_t pages = (fb_bytes + 4095) / 4096;
  if (pages > FB_MAX_PAGES) {
    log_write_hex("FB: MB2 buffer too large, pages =", pages, KERNEL,
                  LOG_ERROR);
    return -1;
  }
  /* Synthesise a page list from the contiguous MMIO base so that sys_fb_map
   * (and the eventual switch to virtio) can iterate without caring which
   * mode we're in. */
  for (uint32_t i = 0; i < pages; i++) {
    fb_pages[i] = t->addr + (uint64_t)i * 4096;
  }
  fb_n_pages = (uint32_t)pages;
  fb_mapped_pages = fb_n_pages;

  if (map_pages_at_vbase(fb_pages, fb_n_pages, /*cacheable=*/0) != 0) {
    log_write("FB: MB2 map failed", KERNEL, LOG_ERROR);
    return -1;
  }
  fb = (uint32_t *)FB_VIRT_BASE;
  fb_mode = FB_MODE_MB2;
  mark_full_damage();

  log_write_hex("DISPLAY: phys      =", t->addr, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: width     =", fb_w, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: height    =", fb_h, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: pitch     =", fb_pitch, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: bpp       =", t->bpp, KERNEL, LOG_INFO);
  return 0;
}

void framebuffer_panic_takeover(void) {
  /* The interrupted context may hold either of these, and it is never going
   * to run again to release them. Reinitialise rather than unlock so the
   * state is well-defined regardless of who held what. */
  spinlock_init(&damage_lock);
  spinlock_init(&scanout_lock);
}

struct gfx_surface framebuffer_get_gfx_surface(void) {
  struct gfx_surface s;
  s.px = framebuffer_buffer();
  s.w = framebuffer_width();
  s.h = framebuffer_height();

  // framebuffer_pitch() returns bytes per row.
  // gfx_surface.stride expects pixels per row.
  s.stride = framebuffer_pitch() / sizeof(uint32_t);

  // Assuming gfx_rect is defined as {x, y, w, h}.
  // Set the clip to the full screen bounds.
  s.clip.x = 0;
  s.clip.y = 0;
  s.clip.w = s.w;
  s.clip.h = s.h;

  return s;
}

/* Tear down the current backing: unmap pages from FB_VIRT_BASE, and in virtio
 * mode also free the underlying PMM frames. MB2 frames are MMIO, not owned by
 * pmm, so leave them alone. */
static void teardown_current(int free_virtio_pages) {
  if (fb_mapped_pages > 0)
    unmap_pages_at_vbase(fb_mapped_pages);
  if (free_virtio_pages && fb_mode == FB_MODE_VIRTIO && fb_pool_phys)
    pmm_free_contiguous(fb_pool_phys, fb_owned_pages);
  fb_n_pages = 0;
  fb_mapped_pages = 0;
  fb_owned_pages = 0;
  fb_pool_phys = 0;
}

/* Allocate one maximum-sized pool. The host resource keeps all of it attached,
 * while the kernel and userspace map only the prefix needed by visible rows. */
static int alloc_and_map_virtio(uint32_t visible_pages) {
  if (visible_pages > FB_MAX_PAGES) {
    log_write_hex("FB: virtio visible span too large, pages =", visible_pages,
                  KERNEL, LOG_ERROR);
    return -1;
  }

  /* One contiguous allocation avoids O(n^2) repeated first-fit scans and
   * guarantees RESOURCE_ATTACH_BACKING fits in one memory entry. */
  fb_pool_phys = pmm_alloc_contiguous_below(UINT64_MAX, FB_MAX_PAGES);
  if (!fb_pool_phys) {
    log_write("FB: no contiguous 64 MiB backing pool", KERNEL, LOG_ERROR);
    return -1;
  }
  for (uint32_t i = 0; i < FB_MAX_PAGES; i++)
    fb_pages[i] = fb_pool_phys + (uint64_t)i * 4096;
  fb_owned_pages = FB_MAX_PAGES;

  if (map_pages_at_vbase(fb_pages, visible_pages, 1) != 0) {
    log_write("FB: virtio map failed", KERNEL, LOG_ERROR);
    pmm_free_contiguous(fb_pool_phys, FB_MAX_PAGES);
    fb_pool_phys = 0;
    fb_owned_pages = 0;
    return -1;
  }
  fb_n_pages = visible_pages;
  fb_mapped_pages = visible_pages;
  return 0;
}

static uint32_t visible_page_count(uint32_t pitch, uint32_t h) {
  uint64_t bytes = (uint64_t)pitch * (uint64_t)h;
  return (uint32_t)((bytes + 4095) / 4096);
}

/* Resize growth maps only the newly-visible suffix. Shrinking leaves mappings
 * in place so a later grow does not repeat page-table work. */
static int ensure_visible_pages(uint32_t pages) {
  if (pages > fb_owned_pages)
    return -1;
  while (fb_mapped_pages < pages) {
    uint32_t i = fb_mapped_pages;
    if (vmm_map(FB_VIRT_BASE + (uint64_t)i * 4096, fb_pages[i],
                VMM_PRESENT | VMM_WRITE) != 0)
      return -1;
    fb_mapped_pages++;
  }
  return 0;
}

static uint32_t resource_dimension(uint32_t value, uint32_t minimum) {
  uint32_t result = minimum;
  while (result < value && result <= UINT32_MAX / 2)
    result *= 2;
  return result;
}

/* Reserve power-of-two headroom so a normal drag changes only SET_SCANOUT.
 * If rounding would exceed the 64 MiB pool, use the exact dimensions. */
static int choose_resource_size(uint32_t w, uint32_t h, uint32_t *resource_w,
                                uint32_t *resource_h) {
  if (w == 0 || h == 0 || (uint64_t)w * (uint64_t)h * 4 > FB_MAX_BYTES)
    return -1;
  uint32_t rounded_w = resource_dimension(w, FB_MIN_RESOURCE_W);
  uint32_t rounded_h = resource_dimension(h, FB_MIN_RESOURCE_H);
  if (rounded_w < w || rounded_h < h ||
      (uint64_t)rounded_w * (uint64_t)rounded_h * 4 > FB_MAX_BYTES) {
    *resource_w = w;
    *resource_h = h;
  } else {
    *resource_w = rounded_w;
    *resource_h = rounded_h;
  }
  return 0;
}

static uint32_t resource_page_count(uint32_t w, uint32_t h) {
  return visible_page_count(w * 4, h);
}

static int do_resize(uint32_t w, uint32_t h) {
  if (fb_mode != FB_MODE_VIRTIO)
    return -1;
  uint32_t resource_w = fb_resource_w;
  uint32_t resource_h = fb_resource_h;
  int recreate = w > resource_w || h > resource_h;
  if (recreate && choose_resource_size(w, h, &resource_w, &resource_h) != 0)
    return -1;

  uint32_t pitch = resource_w * 4;
  uint32_t pages = visible_page_count(pitch, h);
  if (ensure_visible_pages(pages) != 0)
    return -1;

  int rc;
  if (recreate) {
    uint32_t resource_pages = resource_page_count(resource_w, resource_h);
    rc = virtio_gpu_create_scanout_2d(resource_w, resource_h, w, h, fb_pages,
                                      resource_pages);
  } else {
    rc = virtio_gpu_resize_scanout_2d(w, h);
  }
  if (rc != 0) {
    log_write("FB: virtio resize scanout failed", KERNEL, LOG_ERROR);
    return -1;
  }

  fb_n_pages = pages;
  fb_resource_w = resource_w;
  fb_resource_h = resource_h;
  fb_w = w;
  fb_h = h;
  fb_pitch = pitch;
  fb = (uint32_t *)FB_VIRT_BASE;
  mouse_set_bounds(fb_w, fb_h);
  mark_full_damage();
  fb_resize_generation++;
  return 0;
}

int framebuffer_attach_virtio(void) {
  uint32_t w = 0, h = 0;
  if (virtio_gpu_get_dims(&w, &h) != 0)
    return -1;
  uint32_t resource_w = 0, resource_h = 0;
  if (choose_resource_size(w, h, &resource_w, &resource_h) != 0)
    return -1;
  uint32_t pitch = resource_w * 4;
  uint32_t pages = visible_page_count(pitch, h);

  teardown_current(/*free_virtio_pages=*/1);
  if (alloc_and_map_virtio(pages) != 0)
    return -1;

  uint32_t resource_pages = resource_page_count(resource_w, resource_h);
  if (virtio_gpu_create_scanout_2d(resource_w, resource_h, w, h, fb_pages,
                                   resource_pages) != 0) {
    log_write("FB: virtio set_scanout failed", KERNEL, LOG_ERROR);
    return -1;
  }

  fb = (uint32_t *)FB_VIRT_BASE;
  fb_w = w;
  fb_h = h;
  fb_pitch = pitch;
  fb_resource_w = resource_w;
  fb_resource_h = resource_h;
  fb_mode = FB_MODE_VIRTIO;
  mark_full_damage();
  log_write_hex("FB: virtio attached, w =", fb_w, KERNEL, LOG_INFO);
  log_write_hex("FB: virtio attached, h =", fb_h, KERNEL, LOG_INFO);
  return 0;
}

int framebuffer_check_resize(void) {
  if (fb_mode != FB_MODE_VIRTIO)
    return 0;
  if (!virtio_gpu_poll_display_event())
    return 0;

  uint32_t w = 0, h = 0;
  if (virtio_gpu_get_dims(&w, &h) != 0)
    return 0;
  if (w == fb_w && h == fb_h)
    return 0;
  return do_resize(w, h) == 0;
}

void framebuffer_present(void) {
  if (fb_mode != FB_MODE_VIRTIO)
    return;
  /* Snapshot + clear before the synchronous flush so writers can continue
   * accumulating the next batch while virtio waits for host acknowledgement. */
  struct fb_damage_rect pending[FB_MAX_DAMAGE];
  spin_lock(&damage_lock);
  if (dmg_count == 0) {
    spin_unlock(&damage_lock);
    return;
  }
  int count = dmg_count;
  if (count > FB_MAX_DAMAGE)
    count = FB_MAX_DAMAGE;
  for (int i = 0; i < count; i++)
    pending[i] = dmg[i];
  dmg_count = 0;
  spin_unlock(&damage_lock);

  spin_lock(&scanout_lock);
  for (int i = 0; i < count; i++) {
    virtio_gpu_flush_rect(pending[i].x, pending[i].y, pending[i].w,
                          pending[i].h);
  }
  spin_unlock(&scanout_lock);
}

uint32_t framebuffer_resize_generation(void) { return fb_resize_generation; }

void framebuffer_flush_thread_entry(void) {
  for (;;) {
    framebuffer_check_resize();
    framebuffer_present();
    task_sleep_ticks(1); /* 100 Hz upper bound on flush rate */
  }
}

/* The simplest possible "graphics" API: one pixel, one store, no batching,
 * no surface, no DMA. If anyone tries to build a real UI on top of putpixel,
 * we're going to see exactly the framerate it deserves. */
void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color) {
  if (x >= fb_w || y >= fb_h)
    return;
  uint32_t *row = (uint32_t *)((uint8_t *)fb + y * fb_pitch);
  row[x] = color;
  framebuffer_mark_damage(x, y, 1, 1);
}

void framebuffer_clear(uint32_t color) {
  /* Fast path when pitch is tight (no padding between rows): the whole
   * framebuffer is contiguous, do one long stosd-shaped sweep. The slow
   * path still walks rows so we don't trample padding bytes some
   * pre-2010s GPU decided it cared about. */
  if (fb_pitch == fb_w * 4) {
    uint32_t *p = fb;
    uint32_t n = fb_w * fb_h;
    for (uint32_t i = 0; i < n; i++)
      p[i] = color;
    return;
  }
  /* Strength-reduced: advance row pointer by pitch instead of multiplying
   * y*pitch every row. */
  uint8_t *p = (uint8_t *)fb;
  for (uint32_t y = 0; y < fb_h; y++) {
    uint32_t *row = (uint32_t *)p;
    for (uint32_t x = 0; x < fb_w; x++)
      row[x] = color;
    p += fb_pitch;
  }
  mark_full_damage();
}
