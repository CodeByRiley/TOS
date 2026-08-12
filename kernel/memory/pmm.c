/* kernel/memory/pmm.c — bitmap physical-frame allocator.
 *
 * Single bitmap covering [0, highest_usable_addr) at 4 KiB granularity.
 * 1 = frame is taken / reserved; 0 = frame is free. A next-fit hint keeps
 * sequential page allocations linear overall instead of restarting the
 * bitmap scan for every frame.
 *
 * Build order: at boot we mark EVERYTHING used, then walk the MB2 mmap
 * to flip "usable" regions free, then re-mark the bits we still need
 * (kernel image, GRUB info, the bitmap itself, MB2 modules).
 */
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "arch/cpu.h"
#include "boot/multiboot2.h"
#include "devices/serial.h"
#include "utilities/log.h"
#include <stdint.h>

#define GIB (1ULL << 30)
#define PAGE_2M (1ULL << 21)
#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITE (1ULL << 1)
#define PAGE_HUGE (1ULL << 7)
#define PAGE_FLAGS (PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE)
#define PAGE_ADDR_MASK 0x000ffffffffff000ULL
#define BOOT_HHDM_GIB 4ULL
#define BOOT_HHDM_LIMIT (BOOT_HHDM_GIB * GIB)
#define PMM_GENERAL_ALLOC_BASE 0x100000ULL
#define PMM_ISA_DMA_START 0x10000ULL
#define PMM_ISA_DMA_END 0xA0000ULL

#define u8t uint8_t
#define u16t uint16_t
#define u64t uint64_t

extern char _kernel_start[];
extern char _kernel_end[];

extern char _kernel_phys_start[];
extern char _kernel_phys_end[];

static u8t *bitmap = 0;
static u64t bitmap_frames = 0;
static u64t usable_frames = 0;
static u64t used_frames = 0;
static u64t next_free_frame = PMM_GENERAL_ALLOC_BASE / FRAME_SIZE;

static void bitmap_set(u64t frame) { bitmap[frame / 8] |= (1 << (frame % 8)); }
static void bitmap_clear(u64t frame) {
  bitmap[frame / 8] &= ~(1 << (frame % 8));
}
static int bitmap_test(u64t frame) {
  return bitmap[frame / 8] & (1 << (frame % 8));
}

static u64t align_up(u64t value, u64t align) {
  return (value + align - 1) & ~(align - 1);
}

static u64t max_u64(u64t a, u64t b) { return a > b ? a : b; }

/* Find a frame-aligned spot in some usable mmap region that's at least
 * `bitmap_bytes` long and starts at or above `min_addr`. Used to place
 * the bitmap itself away from the kernel image + GRUB info. */
static u64t find_bitmap_region(struct MB2_TAG_MMAP *mmap, u8t *end,
                               u64t bitmap_bytes, u64t min_addr) {
  for (struct MB2_MMAP_ENTRY *e = mmap->entries; (u8t *)e < end;
       e = (struct MB2_MMAP_ENTRY *)((u8t *)e + mmap->entry_size)) {
    if (e->type != 1)
      continue;

    u64t region_top = e->base + e->len;
    u64t candidate = align_up(max_u64(e->base, min_addr), FRAME_SIZE);

    if (candidate < region_top && bitmap_bytes <= region_top - candidate) {
      return candidate;
    }
  }

  return 0;
}

/* Mark [base, base+length) as taken — rounds outward to whole frames. */
static void mark_region_used(u64t base, u64t length) {
  u64t start = base / FRAME_SIZE;
  u64t end = (base + length + FRAME_SIZE - 1) / FRAME_SIZE;
  for (u64t f = start; f < end && f < bitmap_frames; f++) {
    if (!bitmap_test(f)) {
      bitmap_set(f);
      used_frames++;
    }
  }
}

/* Mark [base, base+length) as free — rounds inward to whole frames. */
static void mark_region_free(u64t base, u64t length) {
  u64t start = base / FRAME_SIZE;
  u64t end = (base + length) / FRAME_SIZE;
  for (u64t f = start; f < end && f < bitmap_frames; f++) {
    if (bitmap_test(f)) {
      bitmap_clear(f);
      used_frames--;
    }
  }
}

static void hhdm_extend(u64t highest_physical) {
  u64t required_gib = (highest_physical + GIB - 1) / GIB;

  /* PML4[256] can contain at most 512 PDPT entries = 512 GiB. */
  if (required_gib > 512) {
    log_write("PMM: HHDM exceeds 512 GiB", KERNEL, LOG_ERROR);
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  u64t cr3 = read_cr3();
  u64t *pml4 = phys_to_virt(cr3 & PAGE_ADDR_MASK);
  u64t *hhdm_pdpt = phys_to_virt(pml4[256] & PAGE_ADDR_MASK);

  /* Entries 0..3 were created by the bootstrap 4 GiB HHDM. */
  for (uint64_t gib = BOOT_HHDM_GIB; gib < required_gib; gib++) {
    uint64_t pd_phys = pmm_alloc_frame_below(BOOT_HHDM_LIMIT);
    if (!pd_phys) {
      log_write("PMM: no low frame for HHDM page table", KERNEL, LOG_ERROR);
      for (;;)
        __asm__ volatile("cli; hlt");
    }

    uint64_t *pd = phys_to_virt(pd_phys);

    for (uint64_t entry = 0; entry < 512; entry++) {
      uint64_t phys = gib * GIB + entry * PAGE_2M;
      pd[entry] = phys | PAGE_FLAGS;
    }

    hhdm_pdpt[gib] = pd_phys | PAGE_PRESENT | PAGE_WRITE;
  }

  write_cr3(cr3); /* flush cached translations */
}

/* Parse the MB2 mmap, place + populate the bitmap, then reserve regions
 * the kernel + bootloader still need. Halts on any unrecoverable error
 * (no usable RAM, no place to put the bitmap). */
void pmm_init(u64t mb2_addr) {
  struct MB2_TAG_MMAP *mmap =
      (struct MB2_TAG_MMAP *)mb2_find_tag(mb2_addr, MULTIBOOT_TAG_MMAP);
  if (!mmap) {
    log_write("PMM: no mmap tag", KERNEL, LOG_ERROR);
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  /* Find the highest usable address (so the bitmap covers all RAM,
   * even with high reserved MMIO holes). Also sum usable bytes so we
   * can report real RAM separately from bitmap span. */
  u64t highest_usable_addr = 0;
  u64t usable_bytes = 0;
  u8t *end = (u8t *)mmap + mmap->size;
  for (struct MB2_MMAP_ENTRY *e = mmap->entries; (u8t *)e < end;
       e = (struct MB2_MMAP_ENTRY *)((u8t *)e + mmap->entry_size)) {
    if (e->type != 1)
      continue;
    u64t top = e->base + e->len;
    if (top > highest_usable_addr)
      highest_usable_addr = top;
    usable_bytes += e->len;
  }
  usable_frames = usable_bytes / FRAME_SIZE;
  if (highest_usable_addr == 0) {
    log_write("PMM: no usable memory regions", KERNEL, LOG_ERROR);
    for (;;)
      __asm__ volatile("cli; hlt");
  }
  log_write_int("PMM: highest usable address: ", highest_usable_addr, KERNEL,
                LOG_INFO);
  bitmap_frames = highest_usable_addr / FRAME_SIZE;
  u64t bitmap_bytes = (bitmap_frames + 7) / 8;

  /* Place the bitmap clear of the kernel + GRUB info + below-1MB BIOS. */
  u64t mb2_size = *(uint32_t *)phys_to_virt(mb2_addr);
  u64t min_bitmap_addr =
      max_u64((uintptr_t)_kernel_phys_end, mb2_addr + mb2_size);
  min_bitmap_addr = max_u64(min_bitmap_addr, 0x100000);

  /* Exclude every multiboot module range from bitmap placement. */
  {
    u8t *p = (u8t *)phys_to_virt(mb2_addr) + 8;
    while (1) {
      struct MB2_TAG *t = (struct MB2_TAG *)p;
      if (t->type == MULTIBOOT_TAG_END)
        break;
      if (t->type == MULTIBOOT_TAG_MODULE) {
        struct MB2_TAG_MODULE *m = (struct MB2_TAG_MODULE *)t;
        min_bitmap_addr = max_u64(min_bitmap_addr, m->mod_end);
      }
      p += (t->size + 7) & ~7;
    }
  }

  u64t bitmap_phys =
      find_bitmap_region(mmap, end, bitmap_bytes, min_bitmap_addr);
  if (bitmap_phys == 0) {
      log_write("PMM: no usable region for bitmap", KERNEL, LOG_ERROR);
      for (;;)
          __asm__ volatile("cli; hlt");
  }

  if (bitmap_phys >= BOOT_HHDM_LIMIT ||
      bitmap_bytes > BOOT_HHDM_LIMIT - bitmap_phys) {
      log_write("PMM: bitmap lies outside bootstrap HHDM", KERNEL, LOG_ERROR);
      for (;;)
          __asm__ volatile("cli; hlt");
  }
  bitmap = phys_to_virt(bitmap_phys);
  // bitmap = (u8t *)bitmap_phys;
  /* Mark everything used, then flip back the usable regions and re-mark
   * what the kernel/bootloader still need. */
  for (u64t i = 0; i < bitmap_bytes; i++)
    bitmap[i] = 0xFF;
  used_frames = bitmap_frames;

  for (struct MB2_MMAP_ENTRY *e = mmap->entries; (u8t *)e < end;
       e = (struct MB2_MMAP_ENTRY *)((u8t *)e + mmap->entry_size)) {
    if (e->type == 1)
      mark_region_free(e->base, e->len);
  }

  /* Mark standard low memory BIOS regions as used, but leave the
   * 0x10000 - 0xA0000 range free for ISA DMA bounce buffers. Generic
   * frame allocation starts at 1 MiB, so this pool stays intact. */
  mark_region_used(0, PMM_ISA_DMA_START); /* Real mode IVT + BDA */
  mark_region_used(PMM_ISA_DMA_END,
                   PMM_GENERAL_ALLOC_BASE - PMM_ISA_DMA_END);
  mark_region_used((uintptr_t)_kernel_phys_start,
                   (uintptr_t)_kernel_phys_end - (uintptr_t)_kernel_phys_start);
  mark_region_used(bitmap_phys, bitmap_bytes); /* bitmap itself     */
  mark_region_used(mb2_addr, mb2_size);        /* bootloader info   */

  /* Reserve every multiboot module so its payload is preserved. */
  {
    u8t *p = (u8t *)phys_to_virt(mb2_addr) + 8;
    while (1) {
      struct MB2_TAG *t = (struct MB2_TAG *)p;
      if (t->type == MULTIBOOT_TAG_END)
        break;
      if (t->type == MULTIBOOT_TAG_MODULE) {
        struct MB2_TAG_MODULE *m = (struct MB2_TAG_MODULE *)t;
        mark_region_used(m->mod_start, m->mod_end - m->mod_start);
        log_write_hex("PMM: module @", m->mod_start, KERNEL, LOG_INFO);
      }
      p += (t->size + 7) & ~7;
    }
  }

  /* The direct map covers RAM. Reserved PCI/MMIO windows may live far above
   * RAM and are mapped explicitly by their owning drivers instead. */
  hhdm_extend(highest_usable_addr);
  log_write_hex("PMM: total frames =", bitmap_frames, KERNEL, LOG_INFO);
  log_write_hex("PMM: usable frames=", usable_frames, KERNEL, LOG_INFO);
  log_write_hex("PMM: used frames  =", used_frames, KERNEL, LOG_INFO);
}

static u64t allocate_frame_at(u64t frame) {
  bitmap_set(frame);
  used_frames++;
  next_free_frame = frame + 1;
  if (next_free_frame >= bitmap_frames)
    next_free_frame = PMM_GENERAL_ALLOC_BASE / FRAME_SIZE;
  return frame * FRAME_SIZE;
}

/* Next-fit scan. The wraparound preserves first-fit completeness while large
 * eager mmap calls normally advance one bit per allocation. */
u64t pmm_alloc_frame(void) {
  u64t first = PMM_GENERAL_ALLOC_BASE / FRAME_SIZE;
  u64t start = next_free_frame;
  if (start < first || start >= bitmap_frames)
    start = first;

  for (u64t f = start; f < bitmap_frames; f++)
    if (!bitmap_test(f))
      return allocate_frame_at(f);
  for (u64t f = first; f < start; f++)
    if (!bitmap_test(f))
      return allocate_frame_at(f);

  log_write("PMM: out of memory", KERNEL, LOG_ERROR);
  return 0;
}

u64t pmm_alloc_frame_below(u64t limit) {
  u64t frames = limit / FRAME_SIZE;
  if (frames > bitmap_frames)
    frames = bitmap_frames;

  u64t first_frame = 0;
  if (limit > PMM_GENERAL_ALLOC_BASE)
    first_frame = PMM_GENERAL_ALLOC_BASE / FRAME_SIZE;

  for (u64t f = first_frame; f < frames; f++) {
    if (!bitmap_test(f)) {
      bitmap_set(f);
      used_frames++;
      if (f >= PMM_GENERAL_ALLOC_BASE / FRAME_SIZE) {
        next_free_frame = f + 1;
        if (next_free_frame >= bitmap_frames)
          next_free_frame = PMM_GENERAL_ALLOC_BASE / FRAME_SIZE;
      }
      return f * FRAME_SIZE;
    }
  }

  log_write("PMM: no free frame below limit", KERNEL, LOG_ERROR);
  return 0;
}

/* Finds N contiguous physical frames below a given physical address limit.
 * Returns the physical base address, or 0 on failure. */
u64t pmm_alloc_contiguous_below(u64t limit, u64t num_frames) {
  if (num_frames == 0)
    return 0;

  u64t max_frames = limit / FRAME_SIZE;
  if (max_frames > bitmap_frames)
    max_frames = bitmap_frames;

  /* Same rule as pmm_alloc_frame_below: only a caller whose limit sits at
   * or below 1 MiB is asking for the ISA DMA pool. Anyone asking for more
   * headroom gets general frames, so low memory is not handed out to
   * callers that never needed it. */
  u64t first_frame = 0;
  if (limit > PMM_GENERAL_ALLOC_BASE)
    first_frame = PMM_GENERAL_ALLOC_BASE / FRAME_SIZE;

  u64t consecutive = 0;
  u64t start_frame = 0;

  for (u64t f = first_frame; f < max_frames; f++) {
    if (!bitmap_test(f)) {
      if (consecutive == 0)
        start_frame = f;
      consecutive++;

      if (consecutive == num_frames) {
        for (u64t i = start_frame; i < start_frame + num_frames; i++) {
          bitmap_set(i);
          used_frames++;
        }
        if (start_frame >= PMM_GENERAL_ALLOC_BASE / FRAME_SIZE) {
          next_free_frame = start_frame + num_frames;
          if (next_free_frame >= bitmap_frames)
            next_free_frame = PMM_GENERAL_ALLOC_BASE / FRAME_SIZE;
        }
        return start_frame * FRAME_SIZE;
      }
    } else {
      consecutive = 0;
    }
  }

  log_write("PMM: no contiguous frames below limit", KERNEL, LOG_ERROR);
  return 0;
}

/* Counterpart to pmm_alloc_contiguous_below. `frame` is the base returned
 * by that call and `num_frames` the same count that was requested. */
void pmm_free_contiguous(u64t frame, u64t num_frames) {
  for (u64t i = 0; i < num_frames; i++)
    pmm_free_frame(frame + i * FRAME_SIZE);
}

void pmm_free_frame(u64t frame) {
  u64t f = frame / FRAME_SIZE;
  if (bitmap_test(f)) {
    bitmap_clear(f);
    used_frames--;
    if (f >= PMM_GENERAL_ALLOC_BASE / FRAME_SIZE && f < next_free_frame)
      next_free_frame = f;
  }
}

u64t pmm_total_frames(void) { return bitmap_frames; }
u64t pmm_usable_frames(void) { return usable_frames; }
u64t pmm_used_frames(void) { return used_frames; }
