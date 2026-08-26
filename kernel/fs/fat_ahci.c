/* AHCI backend. Mounts the FAT image in RAM and writes changes through. */
#include "drivers/storage/ahci.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "memory/vmm.h"
#include <fs/fat.h>
#include <fs/fat_ahci.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* Maximum sectors per AHCI request; physical gaps may reduce it. */
#define FAT_AHCI_CHUNK 64
#define PAGE_SIZE 4096u

extern struct AHCI_DEVICE_DATA *g_ahci_dev;

/* Heap addresses need page-table translation because they are outside HHDM. */
static uint64_t dma_phys(const void *virt) {
    return vmm_translate((uint64_t)(uintptr_t)virt);
}

/* Limit DMA to physically contiguous pages. `base` must be page-aligned. */
static uint32_t dma_run_sectors(const uint8_t *base, uint32_t max_sectors,
                                uint32_t bytes_per_sector) {
    if (!bytes_per_sector || !max_sectors)
        return 0;

    uint64_t first = dma_phys(base);
    if (!first)
        return 0;

    uint32_t per_page = PAGE_SIZE / bytes_per_sector;
    if (per_page == 0)
        per_page = 1; /* sector larger than a page: one at a time */

    uint32_t run = per_page;
    while (run < max_sectors) {
        const uint8_t *next = base + (size_t)run * bytes_per_sector;
        if (dma_phys(next) != first + (uint64_t)run * bytes_per_sector)
            break;
        run += per_page;
    }
    return run > max_sectors ? max_sectors : run;
}

/* Prevent writes when the image came from the Multiboot fallback. */
static int ahci_backed;

/* Write-through callback used by fat.c. */
static void ahci_sector_writer(uint32_t lba, void *data) {
    if (!g_ahci_dev)
        return;

    uint64_t phys = dma_phys(data);
    if (!phys)
        return;
    ahci_write_sector(g_ahci_dev, 0, lba, 1, (void *)phys);
}

int fat_mount_from_ahci(struct AHCI_DEVICE_DATA *ahci_dev, int port) {
    if (!ahci_dev) return -1;

    uint8_t *bpb_buf = kmalloc(512);
    if (!bpb_buf) return -1;

    uint64_t bpb_phys = dma_phys(bpb_buf);
    if (!bpb_phys ||
        ahci_read_sector(ahci_dev, port, 0, 1, (void*)bpb_phys) != 0) {
        kfree(bpb_buf);
        return -1;
    }

    uint32_t bytes_per_sector = 0;
    size_t disk_size = fat_volume_size(bpb_buf, &bytes_per_sector);
    if (!disk_size) {
        kfree(bpb_buf);
        return -1;
    }
    uint32_t total_sectors = (uint32_t)(disk_size / bytes_per_sector);

    /* Page alignment keeps DMA run boundaries aligned with frames. */
    uint8_t *ram_raw = kmalloc(disk_size + PAGE_SIZE);
    if (!ram_raw) {
        kfree(bpb_buf);
        return -1;
    }
    uint8_t *ram_disk =
        (uint8_t *)(((uintptr_t)ram_raw + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1));

    /* Read the image without crossing physical gaps. */
    uint32_t sectors_read = 0;
    while (sectors_read < total_sectors) {
        uint8_t *dst = ram_disk + (size_t)sectors_read * bytes_per_sector;
        uint32_t left = total_sectors - sectors_read;
        uint32_t want = left < FAT_AHCI_CHUNK ? left : FAT_AHCI_CHUNK;

        uint32_t chunk = dma_run_sectors(dst, want, bytes_per_sector);
        uint64_t buf_phys = dma_phys(dst);
        if (chunk == 0 || !buf_phys ||
            ahci_read_sector(ahci_dev, port, sectors_read, chunk,
                             (void*)buf_phys) != 0) {
            kfree(ram_raw);
            kfree(bpb_buf);
            return -1;
        }
        sectors_read += chunk;
    }

    int ret = fat_init(ram_disk, disk_size);

    /* Enable write-through only after a successful mount. */
    if (ret == 0) {
        ahci_backed = 1;
        fat_set_sector_writer(ahci_sector_writer);
    } else {
        kfree(ram_raw);
    }

    kfree(bpb_buf);
    return ret;
}

void fat_flush(void) {
    size_t image_size = 0;
    uint32_t bytes_per_sector = 0;
    uint8_t *image = fat_image_base(&image_size, &bytes_per_sector);
    if (!image || !bytes_per_sector || !g_ahci_dev || !ahci_backed) return;

    uint32_t total_sectors = (uint32_t)(image_size / bytes_per_sector);
    uint32_t sectors_written = 0;

    while (sectors_written < total_sectors) {
        uint8_t *src = image + (size_t)sectors_written * bytes_per_sector;
        uint32_t left = total_sectors - sectors_written;
        uint32_t want = left < FAT_AHCI_CHUNK ? left : FAT_AHCI_CHUNK;

        /* BUG: Crossing a physical gap would write unrelated memory. */
        uint32_t chunk = dma_run_sectors(src, want, bytes_per_sector);
        uint64_t buf_phys = dma_phys(src);

        if (chunk == 0 || !buf_phys ||
            ahci_write_sector(g_ahci_dev, 0, sectors_written, chunk,
                              (void*)buf_phys) != 0) {
            log_write("FAT: Flush failed!", KERNEL, LOG_ERROR);
            return;
        }
        sectors_written += chunk;
    }
    log_write("FAT: Disk flushed to AHCI.", KERNEL, LOG_INFO);
}

int fat_read_sector(uint32_t lba, void *buf) {
    if (!g_ahci_dev) return -1;

    void *dma_buf = kmalloc(512);
    if (!dma_buf) return -1;
    uint64_t buf_phys = dma_phys(dma_buf);
    if (!buf_phys) {
        kfree(dma_buf);
        return -1;
    }

    if (ahci_read_sector(g_ahci_dev, 0, lba, 1, (void*)buf_phys) != 0) {
        kfree(dma_buf);
        return -1;
    }

    memcpy(buf, dma_buf, 512);
    kfree(dma_buf);
    return 0;
}
