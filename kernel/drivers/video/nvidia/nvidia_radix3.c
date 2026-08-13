/* NVIDIA GSP radix-3 system-memory descriptor construction.
 *
 * Each table page contains 512 raw 64-bit GPU physical addresses. The
 * three table levels point to non-contiguous PMM pages that contain the
 * firmware payload. This mirrors kgspCreateRadix3_IMPL in NVIDIA's open
 * kernel modules; these entries are not x86 page-table entries and carry
 * no present/write flags.
 */
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <drivers/video/nvidia/nvidia_internal.h>
#include <utilities/string.h>
#include <stddef.h>
#include <stdint.h>

#define RADIX_PAGE_SIZE       4096ULL
#define RADIX_ENTRIES_PER_PAGE 512ULL
#define RADIX_TABLE_LEVELS       3
#define RADIX_LAYOUT_LEVELS      4

struct radix_level {
    uint64_t page_count;
    uint64_t first_page;
};

struct nvidia_radix3 {
    uint64_t *page_phys;
    uint64_t total_page_count;
    uint64_t table_page_count;
    uint64_t data_page_count;
    uint64_t data_size;
    uint64_t root_phys;
};

static void release_pages(uint64_t *pages, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        pmm_free_frame(pages[i]);
}

static int calculate_layout(uint64_t size,
                            struct radix_level level[RADIX_LAYOUT_LEVELS],
                            uint64_t *table_pages, uint64_t *total_pages) {
    if (size == 0 || size > UINT64_MAX - (RADIX_PAGE_SIZE - 1))
        return -1;

    memset(level, 0, sizeof(*level) * RADIX_LAYOUT_LEVELS);
    level[3].page_count = (size + RADIX_PAGE_SIZE - 1) / RADIX_PAGE_SIZE;
    for (int i = 3; i > 0; i--) {
        level[i - 1].page_count =
            (level[i].page_count + RADIX_ENTRIES_PER_PAGE - 1)
            / RADIX_ENTRIES_PER_PAGE;
    }
    if (level[0].page_count != 1)
        return -1;

    for (int i = 1; i < RADIX_LAYOUT_LEVELS; i++) {
        if (level[i - 1].first_page > UINT64_MAX - level[i - 1].page_count)
            return -1;
        level[i].first_page =
            level[i - 1].first_page + level[i - 1].page_count;
    }

    *table_pages = level[3].first_page;
    if (*table_pages > UINT64_MAX - level[3].page_count)
        return -1;
    *total_pages = *table_pages + level[3].page_count;
    return 0;
}

static void fill_table_level(const struct nvidia_radix3 *radix,
                             const struct radix_level *parent,
                             const struct radix_level *child) {
    for (uint64_t child_index = 0;
         child_index < child->page_count; child_index++) {
        uint64_t parent_page = parent->first_page
                             + child_index / RADIX_ENTRIES_PER_PAGE;
        uint64_t entry = child_index % RADIX_ENTRIES_PER_PAGE;
        uint64_t *table = phys_to_virt(radix->page_phys[parent_page]);
        table[entry] = radix->page_phys[child->first_page + child_index];
    }
}

static int verify_tables(const struct nvidia_radix3 *radix,
                         const struct radix_level level[RADIX_LAYOUT_LEVELS]) {
    for (int i = 0; i < RADIX_TABLE_LEVELS; i++) {
        for (uint64_t child_index = 0;
             child_index < level[i + 1].page_count; child_index++) {
            uint64_t parent_page = level[i].first_page
                                 + child_index / RADIX_ENTRIES_PER_PAGE;
            uint64_t entry = child_index % RADIX_ENTRIES_PER_PAGE;
            const uint64_t *table =
                phys_to_virt(radix->page_phys[parent_page]);
            if (table[entry]
                != radix->page_phys[level[i + 1].first_page + child_index])
                return -1;
        }
    }
    return 0;
}

int nvidia_radix3_build(struct nvidia_radix3 **out, const void *data,
                        uint64_t size) {
    struct radix_level level[RADIX_LAYOUT_LEVELS];
    uint64_t table_pages;
    uint64_t total_pages;
    uint64_t allocated = 0;

    if (!out || !data)
        return -1;
    *out = 0;
    if (calculate_layout(size, level, &table_pages, &total_pages) != 0
        || total_pages > (uint64_t)(size_t)-1 / sizeof(uint64_t))
        return -1;

    struct nvidia_radix3 *radix = kmalloc(sizeof(*radix));
    if (!radix)
        return -1;
    memset(radix, 0, sizeof(*radix));

    radix->page_phys = kmalloc((size_t)total_pages * sizeof(uint64_t));
    if (!radix->page_phys) {
        kfree(radix);
        return -1;
    }

    for (; allocated < total_pages; allocated++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys)
            goto fail;
        radix->page_phys[allocated] = phys;
        memset(phys_to_virt(phys), 0, RADIX_PAGE_SIZE);
    }

    radix->total_page_count = total_pages;
    radix->table_page_count = table_pages;
    radix->data_page_count = level[3].page_count;
    radix->data_size = size;
    radix->root_phys = radix->page_phys[0];

    for (int i = 0; i < RADIX_TABLE_LEVELS; i++)
        fill_table_level(radix, &level[i], &level[i + 1]);

    const uint8_t *source = (const uint8_t *)data;
    uint64_t remaining = size;
    for (uint64_t i = 0; i < level[3].page_count; i++) {
        uint64_t chunk = remaining < RADIX_PAGE_SIZE
                       ? remaining : RADIX_PAGE_SIZE;
        void *destination =
            phys_to_virt(radix->page_phys[level[3].first_page + i]);
        memcpy(destination, source, (size_t)chunk);
        source += chunk;
        remaining -= chunk;
    }

    if (remaining != 0 || verify_tables(radix, level) != 0)
        goto fail;

    *out = radix;
    return 0;

fail:
    release_pages(radix->page_phys, allocated);
    kfree(radix->page_phys);
    kfree(radix);
    return -1;
}

void nvidia_radix3_destroy(struct nvidia_radix3 *radix) {
    if (!radix)
        return;
    release_pages(radix->page_phys, radix->total_page_count);
    kfree(radix->page_phys);
    kfree(radix);
}

uint64_t nvidia_radix3_root_phys(const struct nvidia_radix3 *radix) {
    return radix ? radix->root_phys : 0;
}

uint64_t nvidia_radix3_data_size(const struct nvidia_radix3 *radix) {
    return radix ? radix->data_size : 0;
}

uint64_t nvidia_radix3_table_pages(const struct nvidia_radix3 *radix) {
    return radix ? radix->table_page_count : 0;
}

uint64_t nvidia_radix3_data_pages(const struct nvidia_radix3 *radix) {
    return radix ? radix->data_page_count : 0;
}
