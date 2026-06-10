/* src/impl/kernel/memory/pmm.c — bitmap physical-frame allocator.
 *
 * Single bitmap covering [0, highest_usable_addr) at 4 KiB granularity.
 * 1 = frame is taken / reserved; 0 = frame is free. Linear first-fit
 * search; allocation is O(n) in the bitmap, but n is small enough on
 * our typical 4-256 MiB targets that this is fine.
 *
 * Build order: at boot we mark EVERYTHING used, then walk the MB2 mmap
 * to flip "usable" regions free, then re-mark the bits we still need
 * (kernel image, GRUB info, the bitmap itself, MB2 modules).
 */
#include "memory/pmm.h"
#include "boot/multiboot2.h"
#include "devices/serial.h"
#include "utilities/log.h"
#include <stdint.h>

extern char _kernel_start[];
extern char _kernel_end[];

static uint8_t *bitmap = 0;
static uint64_t bitmap_frames = 0;
static uint64_t usable_frames = 0;
static uint64_t used_frames = 0;

static void bitmap_set(uint64_t frame) {
    bitmap[frame / 8] |= (1 << (frame % 8));
}
static void bitmap_clear(uint64_t frame) {
    bitmap[frame / 8] &= ~(1 << (frame % 8));
}
static int bitmap_test(uint64_t frame) {
    return bitmap[frame / 8] & (1 << (frame % 8));
}

static uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* Find a frame-aligned spot in some usable mmap region that's at least
 * `bitmap_bytes` long and starts at or above `min_addr`. Used to place
 * the bitmap itself away from the kernel image + GRUB info. */
static uint64_t find_bitmap_region(struct MB2_TAG_MMAP *mmap, uint8_t *end,
                                   uint64_t bitmap_bytes, uint64_t min_addr) {
    for (struct MB2_MMAP_ENTRY *e = mmap->entries; (uint8_t *)e < end;
         e = (struct MB2_MMAP_ENTRY *)((uint8_t *)e + mmap->entry_size)) {
        if (e->type != 1)
            continue;

        uint64_t region_top = e->base + e->len;
        uint64_t candidate = align_up(max_u64(e->base, min_addr), FRAME_SIZE);

        if (candidate < region_top && bitmap_bytes <= region_top - candidate) {
            return candidate;
        }
    }

    return 0;
}

/* Mark [base, base+length) as taken — rounds outward to whole frames. */
static void mark_region_used(uint64_t base, uint64_t length) {
    uint64_t start = base / FRAME_SIZE;
    uint64_t end = (base + length + FRAME_SIZE - 1) / FRAME_SIZE;
    for (uint64_t f = start; f < end && f < bitmap_frames; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
        }
    }
}

/* Mark [base, base+length) as free — rounds inward to whole frames. */
static void mark_region_free(uint64_t base, uint64_t length) {
    uint64_t start = base / FRAME_SIZE;
    uint64_t end = (base + length) / FRAME_SIZE;
    for (uint64_t f = start; f < end && f < bitmap_frames; f++) {
        if (bitmap_test(f)) {
            bitmap_clear(f);
            used_frames--;
        }
    }
}

/* Parse the MB2 mmap, place + populate the bitmap, then reserve regions
 * the kernel + bootloader still need. Halts on any unrecoverable error
 * (no usable RAM, no place to put the bitmap). */
void pmm_init(uint64_t mb2_addr) {
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
    uint64_t highest_usable_addr = 0;
    uint64_t usable_bytes = 0;
    uint8_t *end = (uint8_t *)mmap + mmap->size;
    for (struct MB2_MMAP_ENTRY *e = mmap->entries; (uint8_t *)e < end;
         e = (struct MB2_MMAP_ENTRY *)((uint8_t *)e + mmap->entry_size)) {
        if (e->type != 1)
            continue;
        uint64_t top = e->base + e->len;
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
    log_write_int("PMM: highest usable address: ", highest_usable_addr, KERNEL, LOG_INFO);
    bitmap_frames = highest_usable_addr / FRAME_SIZE;
    uint64_t bitmap_bytes = (bitmap_frames + 7) / 8;

    /* Place the bitmap clear of the kernel + GRUB info + below-1MB BIOS. */
    uint64_t mb2_size = *(uint32_t *)mb2_addr;
    uint64_t min_bitmap_addr =
        max_u64((uint64_t)_kernel_end, mb2_addr + mb2_size);
    min_bitmap_addr = max_u64(min_bitmap_addr, 0x100000);

    /* Exclude every multiboot module range from bitmap placement. */
    {
        uint8_t *p = (uint8_t *)mb2_addr + 8;
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

    uint64_t bitmap_phys =
        find_bitmap_region(mmap, end, bitmap_bytes, min_bitmap_addr);
    if (bitmap_phys == 0) {
        log_write("PMM: no usable region for bitmap", KERNEL, LOG_ERROR);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    bitmap = (uint8_t *)bitmap_phys;

    /* Mark everything used, then flip back the usable regions and re-mark
     * what the kernel/bootloader still need. */
    for (uint64_t i = 0; i < bitmap_bytes; i++)
        bitmap[i] = 0xFF;
    used_frames = bitmap_frames;

    for (struct MB2_MMAP_ENTRY *e = mmap->entries; (uint8_t *)e < end;
         e = (struct MB2_MMAP_ENTRY *)((uint8_t *)e + mmap->entry_size)) {
        if (e->type == 1)
            mark_region_free(e->base, e->len);
    }

    mark_region_used(0, 0x100000);                                  /* first 1 MiB (BIOS) */
    mark_region_used((uint64_t)_kernel_start,
                     (uint64_t)_kernel_end - (uint64_t)_kernel_start);
    mark_region_used(bitmap_phys, bitmap_bytes);                    /* bitmap itself     */
    mark_region_used(mb2_addr, mb2_size);                           /* bootloader info   */

    /* Reserve every multiboot module so its payload is preserved. */
    {
        uint8_t *p = (uint8_t *)mb2_addr + 8;
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

    log_write_hex("PMM: total frames =", bitmap_frames, KERNEL, LOG_INFO);
    log_write_hex("PMM: usable frames=", usable_frames, KERNEL, LOG_INFO);
    log_write_hex("PMM: used frames  =", used_frames, KERNEL, LOG_INFO);
}

/* First-fit linear scan. Returns the frame's physical base, or 0 on OOM. */
uint64_t pmm_alloc_frame(void) {
    for (uint64_t f = 0; f < bitmap_frames; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
            return f * FRAME_SIZE;
        }
    }
    log_write("PMM: out of memory", KERNEL, LOG_ERROR);
    return 0;
}

void pmm_free_frame(uint64_t frame) {
    uint64_t f = frame / FRAME_SIZE;
    if (bitmap_test(f)) {
        bitmap_clear(f);
        used_frames--;
    }
}

uint64_t pmm_total_frames(void)  { return bitmap_frames; }
uint64_t pmm_usable_frames(void) { return usable_frames; }
uint64_t pmm_used_frames(void)   { return used_frames; }
