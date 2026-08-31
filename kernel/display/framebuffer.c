/* kernel/display/framebuffer.c , framebuffer abstraction.
 *
 * Four backends behind a single API:
 *   MB2 direct  , a native 32-bit BGR framebuffer handed to us by GRUB.
 *                  Direct writes are visible on the scanout.
 *   MB2 shadow  , a canonical 32-bit surface converted into a non-native
 *                  VBE/GOP pixel layout when damage is presented.
 *   virtio-gpu  , a scatter-gather pixel buffer attached to the host
 *                  resource. Damage tracking + framebuffer_present()
 *                  flush touched rects over the virtqueue.
 *   VMSVGA      , VirtualBox's 32-bit VMware SVGA II scanout. VMMDev
 *                  supplies resize hints and the SVGA FIFO publishes damage.
 *
 * Page tables map either backend's pages contiguously at FB_VIRT_BASE so
 * pixel writers don't need to care which mode is active. Switching is
 * one-way (MB2 → accelerated backend); on failure MB2 stays live.
 *
 * The flush thread (framebuffer_flush_thread_entry) polls + presents at
 * PIT tick rate so the (synchronous virtio ACK) flush is decoupled from
 * whoever marked damage.
 */
#include <boot/multiboot2.h>
#include <boot/uefi.h>
#include <display/framebuffer.h>
#include <display/graphics.h>
#include <drivers/video/virtualbox/vbox_video.h>
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
#define FB_SCANOUT_VIRT_BASE 0xFFFFE10000000000ULL
/* Upper bound on FB size , 64 MiB. Covers 4K@32bpp (33 MiB) with headroom.
 * Anything bigger fails attach_virtio cleanly rather than corrupting state. */
#define FB_MAX_BYTES (64ULL * 1024 * 1024)
#define FB_MAX_PAGES (FB_MAX_BYTES / 4096)
#define FB_MIN_RESOURCE_W 2048U
#define FB_MIN_RESOURCE_H 2048U

#define FB_MODE_NONE 0
#define FB_MODE_MB2_DIRECT 1
#define FB_MODE_VIRTIO 2
#define FB_MODE_MB2_SHADOW 3
#define FB_MODE_VBOX 4

#define FB_MAX_DAMAGE 8
#define FB_DAMAGE_MERGE_SLACK 8192

struct fb_damage_rect {
  u32 x, y, w, h;
};

static u32 *fb = 0;
static u32 fb_w = 0;
static u32 fb_h = 0;
static u32 fb_pitch = 0; /* bytes per row */
static u32 fb_mode = FB_MODE_NONE;
static u32 fb_resize_generation = 0;

/* Non-native firmware scanouts keep their actual GOP/VBE mapping separate
 * from the canonical 0x00RRGGBB surface exposed to the renderer/userspace. */
static u8 *fb_scanout = 0;
static u32 fb_scanout_pitch = 0;
static u8 fb_scanout_bpp = 0;
static u8 fb_red_pos = 0, fb_red_size = 0;
static u8 fb_green_pos = 0, fb_green_size = 0;
static u8 fb_blue_pos = 0, fb_blue_size = 0;
static u32 fb_scanout_mapped_pages = 0;
static u64 fb_shadow_phys = 0;
static u32 fb_shadow_pages = 0;

/* Damage rects: pending regions to push to the host scanout on next present.
 * A small fixed set keeps thin, distant writes (cursor + drag ghost strips)
 * from being flattened into one huge bounding box. Adjacent/overlapping
 * damage still merges so normal drawing does not burn a slot per pixel. */
static struct fb_damage_rect dmg[FB_MAX_DAMAGE];
static int dmg_count = 0;
static struct spinlock damage_lock = SPINLOCK_INIT;
/* Serializes writes to the guest backing store against VirtIO transfers.
 * Userspace overlays must enter through framebuffer_present_user to take it. */
static struct spinlock scanout_lock = SPINLOCK_INIT;

static u64 damage_area(const struct fb_damage_rect *r) {
  return (u64)r->w * (u64)r->h;
}

static void damage_union(struct fb_damage_rect *out,
                         const struct fb_damage_rect *a,
                         const struct fb_damage_rect *b) {
  u32 x0 = a->x < b->x ? a->x : b->x;
  u32 y0 = a->y < b->y ? a->y : b->y;
  u32 x1a = a->x + a->w, x1b = b->x + b->w;
  u32 y1a = a->y + a->h, y1b = b->y + b->h;
  u32 x1 = x1a > x1b ? x1a : x1b;
  u32 y1 = y1a > y1b ? y1a : y1b;
  out->x = x0;
  out->y = y0;
  out->w = x1 - x0;
  out->h = y1 - y0;
}

static u64 damage_merge_waste(const struct fb_damage_rect *a,
                                   const struct fb_damage_rect *b) {
  struct fb_damage_rect u;
  damage_union(&u, a, b);
  u64 sum = damage_area(a) + damage_area(b);
  u64 area = damage_area(&u);
  return area > sum ? area - sum : 0;
}

void framebuffer_mark_damage(u32 x, u32 y, u32 w, u32 h) {
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
  u64 best_waste = 0;
  for (int i = 0; i < dmg_count; i++) {
    u64 waste = damage_merge_waste(&dmg[i], &r);
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
  u64 least = damage_merge_waste(&dmg[0], &dmg[1]);
  for (int i = 0; i < dmg_count; i++) {
    for (int j = i + 1; j < dmg_count; j++) {
      u64 waste = damage_merge_waste(&dmg[i], &dmg[j]);
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
static u64 fb_pages[FB_MAX_PAGES];
static u32 fb_n_pages = 0;      /* visible span exposed by sys_fb_map */
static u32 fb_mapped_pages = 0; /* prefix mapped at FB_VIRT_BASE */
static u32 fb_owned_pages = 0;  /* PMM frames owned by virtio mode */
static u64 fb_pool_phys = 0;    /* contiguous virtio backing base */
static u32 fb_resource_w = 0;
static u32 fb_resource_h = 0;
static u64 fb_mb2_phys =
    0; /* preserved so drivers can identify the firmware scanout BAR */

u64 framebuffer_phys(void) {
  if (fb_mode == FB_MODE_MB2_DIRECT || fb_mode == FB_MODE_MB2_SHADOW ||
      fb_mode == FB_MODE_VBOX)
    return fb_mb2_phys;
  if (fb_mode == FB_MODE_VIRTIO && fb_n_pages > 0)
    return fb_pages[0];
  return 0;
}
u32 framebuffer_pitch(void) { return fb_pitch; }
u32 framebuffer_width(void) { return fb_w; }
u32 framebuffer_height(void) { return fb_h; }
u32 *framebuffer_buffer(void) { return fb; }
u32 framebuffer_num_pages(void) { return fb_n_pages; }
u64 framebuffer_phys_for_page(u32 idx) {
  if (idx >= fb_n_pages)
    return 0;
  return fb_pages[idx];
}

#define FB_COPY_MAX_LANES 8
#define FB_COPY_PARALLEL_MIN_BYTES (256ULL * 1024ULL)

struct fb_copy_batch {
  u64 *user_pml4;
  u64 source;
  u32 source_pitch;
  const u64 *source_pages;
  u64 source_page_base;
  u32 source_page_count;
  u8 *destination;
  u32 destination_pitch;
  struct fb_rect rects[FB_PRESENT_MAX_RECTS];
  u32 rect_count;
  u32 lane_count;
  volatile u32 remaining;
  volatile int failed;
};

struct fb_copy_job {
  struct fb_copy_batch *batch;
  u32 lane;
};

static volatile int fb_parallel_copy_logged;

struct fb_user_registration {
  u64 *user_pml4;
  int owner_pid;
  u64 source;
  u64 source_bytes;
  u64 page_base;
  u32 source_pitch;
  u32 page_count;
  u64 *pages;
};

static struct fb_user_registration user_registration;
static struct spinlock user_registration_lock = SPINLOCK_INIT;

static int registration_matches(const struct fb_user_registration *registration,
                                u64 *user_pml4, int owner_pid,
                                u64 source, u32 source_pitch,
                                u64 source_bytes) {
  return registration->pages && registration->user_pml4 == user_pml4 &&
         registration->owner_pid == owner_pid &&
         registration->source == source &&
         registration->source_pitch == source_pitch &&
         registration->source_bytes >= source_bytes;
}

int framebuffer_register_user(u64 *user_pml4, int owner_pid,
                              u64 source, u32 source_pitch,
                              u64 source_bytes) {
  if (!user_pml4 || owner_pid <= 0 || !source || source_bytes == 0 ||
      source + source_bytes < source) {
    return -1;
  }

  u64 page_base = source & ~4095ULL;
  u64 last_page = (source + source_bytes - 1) & ~4095ULL;
  u64 page_count64 = (last_page - page_base) / 4096ULL + 1;
  if (page_count64 == 0 || page_count64 > FB_MAX_PAGES)
    return -1;

  u32 page_count = (u32)page_count64;
  u64 *pages = kmalloc((usize)page_count * sizeof(*pages));
  if (!pages)
    return -1;

  for (u32 i = 0; i < page_count; i++) {
    u64 va = page_base + (u64)i * 4096ULL;
    u64 entry = vmm_entry_in(user_pml4, va);
    if (!(entry & VMM_PRESENT) || !(entry & VMM_USER)) {
      kfree(pages);
      return -1;
    }
    pages[i] = entry & VMM_ADDR_MASK;
  }

  spin_lock(&user_registration_lock);
  u64 *old_pages = user_registration.pages;
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

int framebuffer_unregister_user(u64 *user_pml4, int owner_pid) {
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

  u64 *pages = user_registration.pages;
  user_registration = (struct fb_user_registration){0};
  spin_unlock(&user_registration_lock);
  kfree(pages);
  return 0;
}

int framebuffer_user_buffer_registered(u64 *user_pml4, int owner_pid,
                                       u64 source, u32 source_pitch,
                                       u64 source_bytes) {
  spin_lock(&user_registration_lock);
  int matched = registration_matches(&user_registration, user_pml4, owner_pid,
                                     source, source_pitch, source_bytes);
  spin_unlock(&user_registration_lock);
  return matched;
}

int framebuffer_registered_range_overlaps(u64 *user_pml4, u64 base,
                                          u64 bytes) {
  if (!user_pml4 || bytes == 0 || base + bytes < base)
    return 0;

  spin_lock(&user_registration_lock);
  int overlaps = 0;
  if (user_registration.pages && user_registration.user_pml4 == user_pml4) {
    u64 registered_end =
        user_registration.source + user_registration.source_bytes;
    u64 range_end = base + bytes;
    overlaps = base < registered_end && range_end > user_registration.source;
  }
  spin_unlock(&user_registration_lock);
  return overlaps;
}

/* APs do not run the caller's CR3. Registered buffers use a stable physical
 * page snapshot; unregistered callers fall back to walking the PML4 after the
 * syscall layer validates the source span. */
static int copy_user_pixels(const struct fb_copy_batch *batch, u64 source,
                            u8 *destination, u32 bytes) {
  while (bytes > 0) {
    u64 physical;
    if (batch->source_pages) {
      if (source < batch->source_page_base)
        return -1;
      u64 offset = source - batch->source_page_base;
      u64 page = offset / 4096ULL;
      if (page >= batch->source_page_count)
        return -1;
      physical = batch->source_pages[page] + (offset & 4095ULL);
    } else {
      u64 entry = vmm_entry_in(batch->user_pml4, source);
      if (!(entry & VMM_PRESENT) || !(entry & VMM_USER))
        return -1;
      physical = vmm_translate_in(batch->user_pml4, source);
      if (!physical)
        return -1;
    }

    u32 chunk = 4096U - (u32)(source & 4095U);
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

  for (u32 i = 0; i < batch->rect_count; i++) {
    const struct fb_rect *rect = &batch->rects[i];
    for (u32 row = job->lane; row < rect->h; row += batch->lane_count) {
      if (__atomic_load_n(&batch->failed, __ATOMIC_ACQUIRE))
        break;

      u64 source = batch->source +
                        (u64)(rect->y + row) * batch->source_pitch +
                        (u64)rect->x * 4ULL;
      u8 *destination =
          batch->destination +
          (u64)(rect->y + row) * batch->destination_pitch +
          (u64)rect->x * 4ULL;
      if (copy_user_pixels(batch, source, destination, rect->w * 4U) != 0) {
        __atomic_store_n(&batch->failed, 1, __ATOMIC_RELEASE);
        break;
      }
    }
  }

  __atomic_sub_fetch(&batch->remaining, 1, __ATOMIC_ACQ_REL);
}

int framebuffer_present_user(u64 *user_pml4, int owner_pid,
                             u64 source, u32 source_pitch,
                             const struct fb_rect *rects, u32 rect_count) {
  if (!user_pml4 || !source || !rects || rect_count == 0 ||
      rect_count > FB_PRESENT_MAX_RECTS || !fb || fb_w == 0 || fb_h == 0 ||
      source_pitch < fb_w * 4U) {
    return -1;
  }

  struct fb_copy_batch batch = {
      .user_pml4 = user_pml4,
      .source = source,
      .source_pitch = source_pitch,
      .destination = (u8 *)fb,
      .destination_pitch = fb_pitch,
  };

  u64 required_bytes =
      (u64)source_pitch * (fb_h - 1) + (u64)fb_w * 4ULL;
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

  u64 total_bytes = 0;
  u64 total_rows = 0;
  for (u32 i = 0; i < rect_count; i++) {
    struct fb_rect rect = rects[i];
    if (rect.w == 0 || rect.h == 0 || rect.x >= fb_w || rect.y >= fb_h)
      continue;
    if (rect.w > fb_w - rect.x)
      rect.w = fb_w - rect.x;
    if (rect.h > fb_h - rect.y)
      rect.h = fb_h - rect.y;
    batch.rects[batch.rect_count++] = rect;
    total_bytes += (u64)rect.w * 4ULL * rect.h;
    total_rows += rect.h;
  }
  if (batch.rect_count == 0) {
    if (registration_locked)
      spin_unlock(&user_registration_lock);
    return 0;
  }

  u32 lanes = 1;
  int ap_workers = smp_worker_count();
  if (ap_workers > 0 && total_bytes >= FB_COPY_PARALLEL_MIN_BYTES) {
    u64 wanted = (total_bytes + FB_COPY_PARALLEL_MIN_BYTES - 1) /
                      FB_COPY_PARALLEL_MIN_BYTES;
    u64 available = (u64)ap_workers + 1;
    if (wanted > available)
      wanted = available;
    if (wanted > FB_COPY_MAX_LANES)
      wanted = FB_COPY_MAX_LANES;
    if (wanted > total_rows)
      wanted = total_rows;
    lanes = (u32)wanted;
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
  for (u32 lane = 1; lane < lanes; lane++) {
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
    for (u32 i = 0; i < batch.rect_count; i++) {
      const struct fb_rect *rect = &batch.rects[i];
      framebuffer_mark_damage(rect->x, rect->y, rect->w, rect->h);
    }
  }
  return __atomic_load_n(&batch.failed, __ATOMIC_ACQUIRE) ? -1 : 0;
}

static int map_pages_at(u64 virtual_base, const u64 *pages, u32 n,
                        int cacheable) {
  u64 flags = VMM_PRESENT | VMM_WRITE;
  if (!cacheable)
    flags |= VMM_PCD | VMM_PWT;
  for (u32 i = 0; i < n; i++) {
    if (vmm_map(virtual_base + (u64)i * 4096, pages[i], flags) != 0) {
      while (i > 0) {
        i--;
        vmm_unmap(virtual_base + (u64)i * 4096);
      }
      return -1;
    }
  }
  return 0;
}

static void unmap_pages_at(u64 virtual_base, u32 n) {
  for (u32 i = 0; i < n; i++) {
    vmm_unmap(virtual_base + (u64)i * 4096);
  }
}

static int map_contiguous_at(u64 virtual_base, u64 physical, u64 bytes,
                             int cacheable, u32 *mapped_pages) {
  u64 physical_base = physical & ~4095ULL;
  u64 offset = physical & 4095ULL;
  if (bytes == 0 || bytes > UINT64_MAX - offset)
    return -1;
  u64 page_count64 = (offset + bytes + 4095ULL) / 4096ULL;
  if (page_count64 == 0 || page_count64 > FB_MAX_PAGES)
    return -1;

  u64 flags = VMM_PRESENT | VMM_WRITE;
  if (!cacheable)
    flags |= VMM_PCD | VMM_PWT;
  u32 page_count = (u32)page_count64;
  for (u32 i = 0; i < page_count; i++) {
    if (vmm_map(virtual_base + (u64)i * 4096,
                physical_base + (u64)i * 4096, flags) != 0) {
      unmap_pages_at(virtual_base, i);
      return -1;
    }
  }
  *mapped_pages = page_count;
  return 0;
}

static int channel_layout_valid(u8 position, u8 size, u8 bpp) {
  return size > 0 && size <= 8 && position < bpp &&
         (u16)position + size <= bpp;
}

static u32 encode_channel(u8 value, u8 position, u8 size) {
  u32 maximum = (1U << size) - 1U;
  u32 scaled = ((u32)value * maximum + 127U) / 255U;
  return scaled << position;
}

static u32 encode_scanout_pixel(u32 color) {
  return encode_channel((u8)(color >> 16), fb_red_pos, fb_red_size) |
         encode_channel((u8)(color >> 8), fb_green_pos, fb_green_size) |
         encode_channel((u8)color, fb_blue_pos, fb_blue_size);
}

static void present_shadow_rect(const struct fb_damage_rect *rect) {
  u32 scanout_bytes_per_pixel = (fb_scanout_bpp + 7U) / 8U;
  for (u32 y = 0; y < rect->h; y++) {
    const u32 *source =
        (const u32 *)((const u8 *)fb + (u64)(rect->y + y) * fb_pitch) +
        rect->x;
    u8 *destination = fb_scanout +
                      (u64)(rect->y + y) * fb_scanout_pitch +
                      (u64)rect->x * scanout_bytes_per_pixel;
    for (u32 x = 0; x < rect->w; x++) {
      u32 encoded = encode_scanout_pixel(source[x]);
      for (u32 byte = 0; byte < scanout_bytes_per_pixel; byte++)
        destination[byte] = (u8)(encoded >> (byte * 8));
      destination += scanout_bytes_per_pixel;
    }
  }
}

int framebuffer_init(u64 mb2_addr) {
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
  if (!channel_layout_valid(t->red_pos, t->red_size, t->bpp) ||
      !channel_layout_valid(t->green_pos, t->green_size, t->bpp) ||
      !channel_layout_valid(t->blue_pos, t->blue_size, t->bpp)) {
    log_write("FB: unsupported RGB channel layout", KERNEL, LOG_ERROR);
    return -1;
  }

  u32 scanout_bytes_per_pixel = (t->bpp + 7U) / 8U;
  if (t->width == 0 || t->height == 0 ||
      t->width > UINT32_MAX / scanout_bytes_per_pixel ||
      t->pitch < t->width * scanout_bytes_per_pixel) {
    log_write("FB: invalid dimensions or pitch", KERNEL, LOG_ERROR);
    return -1;
  }

  log_write(uefi_booted() ? "FB: source = UEFI GOP"
                          : "FB: source = BIOS VBE",
            KERNEL, LOG_INFO);
  log_write_hex("FB: red_pos   =", t->red_pos, KERNEL, LOG_INFO);
  log_write_hex("FB: green_pos =", t->green_pos, KERNEL, LOG_INFO);
  log_write_hex("FB: blue_pos  =", t->blue_pos, KERNEL, LOG_INFO);

  fb_w = t->width;
  fb_h = t->height;
  fb_mb2_phys = t->addr;
  fb_scanout_pitch = t->pitch;
  fb_scanout_bpp = t->bpp;
  fb_red_pos = t->red_pos;
  fb_red_size = t->red_size;
  fb_green_pos = t->green_pos;
  fb_green_size = t->green_size;
  fb_blue_pos = t->blue_pos;
  fb_blue_size = t->blue_size;

  u64 fb_bytes = (u64)t->pitch * t->height;
  if (fb_bytes == 0 || fb_bytes > FB_MAX_BYTES) {
    log_write_hex("FB: firmware buffer too large, bytes =", fb_bytes, KERNEL,
                  LOG_ERROR);
    return -1;
  }

  int native_layout = t->bpp == 32 && t->red_pos == 16 &&
                      t->red_size == 8 && t->green_pos == 8 &&
                      t->green_size == 8 && t->blue_pos == 0 &&
                      t->blue_size == 8 && (t->addr & 4095ULL) == 0;
  if (native_layout) {
    u32 pages = (u32)((fb_bytes + 4095ULL) / 4096ULL);
    for (u32 i = 0; i < pages; i++)
      fb_pages[i] = t->addr + (u64)i * 4096;
    if (map_pages_at(FB_VIRT_BASE, fb_pages, pages, /*cacheable=*/0) != 0) {
      log_write("FB: direct firmware map failed", KERNEL, LOG_ERROR);
      return -1;
    }
    fb = (u32 *)FB_VIRT_BASE;
    fb_pitch = t->pitch;
    fb_n_pages = pages;
    fb_mapped_pages = pages;
    fb_mode = FB_MODE_MB2_DIRECT;
    log_write("FB: native 32-bit scanout, using direct writes", KERNEL,
              LOG_INFO);
  } else {
    if (map_contiguous_at(FB_SCANOUT_VIRT_BASE, t->addr, fb_bytes,
                          /*cacheable=*/0,
                          &fb_scanout_mapped_pages) != 0) {
      log_write("FB: firmware scanout map failed", KERNEL, LOG_ERROR);
      return -1;
    }
    fb_scanout = (u8 *)(FB_SCANOUT_VIRT_BASE + (t->addr & 4095ULL));

    u64 shadow_bytes = (u64)t->width * 4ULL * t->height;
    u32 pages = (u32)((shadow_bytes + 4095ULL) / 4096ULL);
    fb_shadow_phys = pmm_alloc_contiguous_below(UINT64_MAX, pages);
    if (!fb_shadow_phys) {
      unmap_pages_at(FB_SCANOUT_VIRT_BASE, fb_scanout_mapped_pages);
      fb_scanout_mapped_pages = 0;
      log_write("FB: no memory for format-conversion surface", KERNEL,
                LOG_ERROR);
      return -1;
    }
    fb_shadow_pages = pages;
    for (u32 i = 0; i < pages; i++)
      fb_pages[i] = fb_shadow_phys + (u64)i * 4096;
    if (map_pages_at(FB_VIRT_BASE, fb_pages, pages, /*cacheable=*/1) != 0) {
      pmm_free_contiguous(fb_shadow_phys, fb_shadow_pages);
      fb_shadow_phys = 0;
      fb_shadow_pages = 0;
      unmap_pages_at(FB_SCANOUT_VIRT_BASE, fb_scanout_mapped_pages);
      fb_scanout_mapped_pages = 0;
      log_write("FB: format-conversion surface map failed", KERNEL,
                LOG_ERROR);
      return -1;
    }
    fb = (u32 *)FB_VIRT_BASE;
    fb_pitch = t->width * 4U;
    fb_n_pages = pages;
    fb_mapped_pages = pages;
    fb_mode = FB_MODE_MB2_SHADOW;
    memset(fb, 0, shadow_bytes);
    log_write("FB: non-native scanout, enabled 32-bit conversion surface",
              KERNEL, LOG_INFO);
  }
  mark_full_damage();

  log_write_hex("DISPLAY: phys      =", t->addr, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: width     =", fb_w, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: height    =", fb_h, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: pitch     =", fb_pitch, KERNEL, LOG_INFO);
  log_write_hex("DISPLAY: bpp       =", t->bpp, KERNEL, LOG_INFO);
  if (fb_mode == FB_MODE_MB2_SHADOW)
    framebuffer_present();
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
  s.stride = framebuffer_pitch() / sizeof(u32);

  s.clip.x = 0;
  s.clip.y = 0;
  s.clip.w = s.w;
  s.clip.h = s.h;

  return s;
}

/* Tear down the current backing: unmap pages from FB_VIRT_BASE, and free only
 * PMM-owned shadow/virtio memory. Firmware and VMSVGA frames are MMIO. */
static void teardown_current(int free_virtio_pages) {
  if (fb_mapped_pages > 0)
    unmap_pages_at(FB_VIRT_BASE, fb_mapped_pages);
  if (fb_mode == FB_MODE_MB2_SHADOW) {
    if (fb_scanout_mapped_pages > 0)
      unmap_pages_at(FB_SCANOUT_VIRT_BASE, fb_scanout_mapped_pages);
    if (fb_shadow_phys)
      pmm_free_contiguous(fb_shadow_phys, fb_shadow_pages);
  }
  if (free_virtio_pages && fb_mode == FB_MODE_VIRTIO && fb_pool_phys)
    pmm_free_contiguous(fb_pool_phys, fb_owned_pages);
  fb_n_pages = 0;
  fb_mapped_pages = 0;
  fb_owned_pages = 0;
  fb_pool_phys = 0;
  fb_scanout = 0;
  fb_scanout_mapped_pages = 0;
  fb_shadow_phys = 0;
  fb_shadow_pages = 0;
}

static int vbox_mode_valid(const struct vbox_video_mode *mode) {
  if (!mode || mode->physical == 0 || (mode->physical & 4095ULL) != 0 ||
      mode->width == 0 || mode->height == 0 || mode->bpp != 32 ||
      mode->pitch < mode->width * 4U || mode->red_mask != 0x00FF0000U ||
      mode->green_mask != 0x0000FF00U || mode->blue_mask != 0x000000FFU)
    return 0;
  u64 bytes = (u64)mode->pitch * mode->height;
  return bytes != 0 && bytes <= FB_MAX_BYTES;
}

static int map_vbox_mode(const struct vbox_video_mode *mode,
                         int initial_attach) {
  if (!vbox_mode_valid(mode)) {
    log_write("FB: unsupported VMSVGA framebuffer layout", KERNEL, LOG_ERROR);
    return -1;
  }

  u32 pages = (u32)(((u64)mode->pitch * mode->height + 4095ULL) / 4096ULL);
  int physical_changed = fb_mode != FB_MODE_VBOX ||
                         fb_mb2_phys != mode->physical;
  if (initial_attach) {
    teardown_current(/*free_virtio_pages=*/1);
    physical_changed = 1;
  } else if (physical_changed && fb_mapped_pages > 0) {
    unmap_pages_at(FB_VIRT_BASE, fb_mapped_pages);
    fb_mapped_pages = 0;
  }

  for (u32 i = 0; i < pages; i++)
    fb_pages[i] = mode->physical + (u64)i * 4096ULL;

  if (physical_changed) {
    if (map_pages_at(FB_VIRT_BASE, fb_pages, pages, /*cacheable=*/0) != 0)
      return -1;
    fb_mapped_pages = pages;
  } else {
    while (fb_mapped_pages < pages) {
      u32 i = fb_mapped_pages;
      if (vmm_map(FB_VIRT_BASE + (u64)i * 4096ULL, fb_pages[i],
                  VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT) != 0)
        return -1;
      fb_mapped_pages++;
    }
  }

  fb = (u32 *)FB_VIRT_BASE;
  fb_mb2_phys = mode->physical;
  fb_w = mode->width;
  fb_h = mode->height;
  fb_pitch = mode->pitch;
  fb_n_pages = pages;
  fb_mode = FB_MODE_VBOX;
  if (!initial_attach)
    mouse_set_bounds(fb_w, fb_h);
  mark_full_damage();
  if (!initial_attach)
    fb_resize_generation++;
  return 0;
}

int framebuffer_attach_virtualbox(void) {
  struct vbox_video_mode mode;
  if (vbox_video_get_mode(&mode) != 0 || map_vbox_mode(&mode, 1) != 0)
    return -1;
  log_write_hex("FB: VirtualBox VMSVGA attached, w =", fb_w, KERNEL,
                LOG_INFO);
  log_write_hex("FB: VirtualBox VMSVGA attached, h =", fb_h, KERNEL,
                LOG_INFO);
  return 0;
}

/* Allocate one maximum-sized pool. The host resource keeps all of it attached,
 * while the kernel and userspace map only the prefix needed by visible rows. */
static int alloc_and_map_virtio(u32 visible_pages) {
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
  for (u32 i = 0; i < FB_MAX_PAGES; i++)
    fb_pages[i] = fb_pool_phys + (u64)i * 4096;
  fb_owned_pages = FB_MAX_PAGES;

  if (map_pages_at(FB_VIRT_BASE, fb_pages, visible_pages, 1) != 0) {
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

static u32 visible_page_count(u32 pitch, u32 h) {
  u64 bytes = (u64)pitch * (u64)h;
  return (u32)((bytes + 4095) / 4096);
}

/* Resize growth maps only the newly-visible suffix. Shrinking leaves mappings
 * in place so a later grow does not repeat page-table work. */
static int ensure_visible_pages(u32 pages) {
  if (pages > fb_owned_pages)
    return -1;
  while (fb_mapped_pages < pages) {
    u32 i = fb_mapped_pages;
    if (vmm_map(FB_VIRT_BASE + (u64)i * 4096, fb_pages[i],
                VMM_PRESENT | VMM_WRITE) != 0)
      return -1;
    fb_mapped_pages++;
  }
  return 0;
}

static u32 resource_dimension(u32 value, u32 minimum) {
  u32 result = minimum;
  while (result < value && result <= UINT32_MAX / 2)
    result *= 2;
  return result;
}

/* Reserve power-of-two headroom so a normal drag changes only SET_SCANOUT.
 * If rounding would exceed the 64 MiB pool, use the exact dimensions. */
static int choose_resource_size(u32 w, u32 h, u32 *resource_w,
                                u32 *resource_h) {
  if (w == 0 || h == 0 || (u64)w * (u64)h * 4 > FB_MAX_BYTES)
    return -1;
  u32 rounded_w = resource_dimension(w, FB_MIN_RESOURCE_W);
  u32 rounded_h = resource_dimension(h, FB_MIN_RESOURCE_H);
  if (rounded_w < w || rounded_h < h ||
      (u64)rounded_w * (u64)rounded_h * 4 > FB_MAX_BYTES) {
    *resource_w = w;
    *resource_h = h;
  } else {
    *resource_w = rounded_w;
    *resource_h = rounded_h;
  }
  return 0;
}

static u32 resource_page_count(u32 w, u32 h) {
  return visible_page_count(w * 4, h);
}

static int do_resize(u32 w, u32 h) {
  if (fb_mode != FB_MODE_VIRTIO)
    return -1;
  u32 resource_w = fb_resource_w;
  u32 resource_h = fb_resource_h;
  int recreate = w > resource_w || h > resource_h;
  if (recreate && choose_resource_size(w, h, &resource_w, &resource_h) != 0)
    return -1;

  u32 pitch = resource_w * 4;
  u32 pages = visible_page_count(pitch, h);
  if (ensure_visible_pages(pages) != 0)
    return -1;

  int rc;
  if (recreate) {
    u32 resource_pages = resource_page_count(resource_w, resource_h);
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
  fb = (u32 *)FB_VIRT_BASE;
  mouse_set_bounds(fb_w, fb_h);
  mark_full_damage();
  fb_resize_generation++;
  return 0;
}

int framebuffer_attach_virtio(void) {
  u32 w = 0, h = 0;
  if (virtio_gpu_get_dims(&w, &h) != 0)
    return -1;
  u32 resource_w = 0, resource_h = 0;
  if (choose_resource_size(w, h, &resource_w, &resource_h) != 0)
    return -1;
  u32 pitch = resource_w * 4;
  u32 pages = visible_page_count(pitch, h);

  teardown_current(/*free_virtio_pages=*/1);
  if (alloc_and_map_virtio(pages) != 0)
    return -1;

  u32 resource_pages = resource_page_count(resource_w, resource_h);
  if (virtio_gpu_create_scanout_2d(resource_w, resource_h, w, h, fb_pages,
                                   resource_pages) != 0) {
    log_write("FB: virtio set_scanout failed", KERNEL, LOG_ERROR);
    return -1;
  }

  fb = (u32 *)FB_VIRT_BASE;
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
  if (fb_mode == FB_MODE_VBOX) {
    struct vbox_video_mode mode;
    spin_lock(&scanout_lock);
    int changed = vbox_video_poll_resize(&mode);
    if (changed > 0)
      changed = map_vbox_mode(&mode, 0) == 0;
    spin_unlock(&scanout_lock);
    return changed > 0;
  }

  if (fb_mode != FB_MODE_VIRTIO || !virtio_gpu_poll_display_event())
    return 0;

  u32 w = 0, h = 0;
  if (virtio_gpu_get_dims(&w, &h) != 0)
    return 0;
  if (w == fb_w && h == fb_h)
    return 0;
  return do_resize(w, h) == 0;
}

void framebuffer_present(void) {
  if (fb_mode != FB_MODE_VIRTIO && fb_mode != FB_MODE_MB2_SHADOW &&
      fb_mode != FB_MODE_VBOX)
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
    if (fb_mode == FB_MODE_MB2_SHADOW)
      present_shadow_rect(&pending[i]);
    else if (fb_mode == FB_MODE_VBOX)
      vbox_video_flush_rect(pending[i].x, pending[i].y, pending[i].w,
                            pending[i].h);
    else
      virtio_gpu_flush_rect(pending[i].x, pending[i].y, pending[i].w,
                            pending[i].h);
  }
  spin_unlock(&scanout_lock);
}

int framebuffer_needs_flush(void) {
  return fb_mode == FB_MODE_VIRTIO || fb_mode == FB_MODE_MB2_SHADOW ||
         fb_mode == FB_MODE_VBOX;
}

u32 framebuffer_resize_generation(void) { return fb_resize_generation; }

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
void framebuffer_putpixel(u32 x, u32 y, u32 color) {
  if (x >= fb_w || y >= fb_h)
    return;
  u32 *row = (u32 *)((u8 *)fb + y * fb_pitch);
  row[x] = color;
  framebuffer_mark_damage(x, y, 1, 1);
}

void framebuffer_clear(u32 color) {
  /* Fast path when pitch is tight (no padding between rows): the whole
   * framebuffer is contiguous, do one long stosd-shaped sweep. The slow
   * path still walks rows so we don't trample padding bytes some
   * pre-2010s GPU decided it cared about. */
  if (fb_pitch == fb_w * 4) {
    u32 *p = fb;
    u32 n = fb_w * fb_h;
    for (u32 i = 0; i < n; i++)
      p[i] = color;
    mark_full_damage();
    return;
  }
  /* Strength-reduced: advance row pointer by pitch instead of multiplying
   * y*pitch every row. */
  u8 *p = (u8 *)fb;
  for (u32 y = 0; y < fb_h; y++) {
    u32 *row = (u32 *)p;
    for (u32 x = 0; x < fb_w; x++)
      row[x] = color;
    p += fb_pitch;
  }
  mark_full_damage();
}
