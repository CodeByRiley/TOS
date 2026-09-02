/* kernel/fs/fat/ahci/fat_ahci.c , AHCI persistence for FAT images.
 *
 * Loads a FAT volume into RAM and writes changed sectors back through AHCI.
 */
#include "drivers/storage/ahci.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include "memory/vmm.h"
#include <fs/fat/fat.h>
#include <fs/fat/ahci/fat_ahci.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* Maximum sectors per AHCI request; physical gaps may reduce it. */
#define FAT_AHCI_CHUNK 64
#define PAGE_SIZE 4096u

extern struct AHCI_DEVICE_DATA *g_ahci_dev;

/* Heap addresses need page-table translation because they are outside HHDM. */
static u64 dma_phys(const void *virt) {
    return vmm_translate((u64)(uintptr_t)virt);
}

/* Limit DMA to physically contiguous pages. `base` must be page-aligned. */
static u32 dma_run_sectors(const u8 *base, u32 max_sectors,
                                u32 bytes_per_sector) {
    if (!bytes_per_sector || !max_sectors)
        return 0;

    u64 first = dma_phys(base);
    if (!first)
        return 0;

    u32 per_page = PAGE_SIZE / bytes_per_sector;
    if (per_page == 0)
        per_page = 1; /* sector larger than a page: one at a time */

    u32 run = per_page;
    while (run < max_sectors) {
        const u8 *next = base + (usize)run * bytes_per_sector;
        if (dma_phys(next) != first + (u64)run * bytes_per_sector)
            break;
        run += per_page;
    }
    return run > max_sectors ? max_sectors : run;
}

/* Prevent writes when the image came from the Multiboot fallback. */
static int ahci_backed;

/* Write-through callback used by fat.c. */
static void ahci_sector_writer(u32 lba, const void *data) {
    if (!g_ahci_dev)
        return;

    u64 phys = dma_phys(data);
    if (!phys)
        return;
    ahci_write_sector(g_ahci_dev, 0, lba, 1, (void *)phys);
}

int fat_mount_from_ahci(struct AHCI_DEVICE_DATA *ahci_dev, int port) {
    if (!ahci_dev) return -1;

    u8 *bpb_buf = kmalloc(512);
    if (!bpb_buf) return -1;

    u64 bpb_phys = dma_phys(bpb_buf);
    if (!bpb_phys ||
        ahci_read_sector(ahci_dev, port, 0, 1, (void*)bpb_phys) != 0) {
        kfree(bpb_buf);
        return -1;
    }

    u32 bytes_per_sector = 0;
    usize disk_size = fat_volume_size(bpb_buf, &bytes_per_sector);
    if (!disk_size) {
        kfree(bpb_buf);
        return -1;
    }
    u32 total_sectors = (u32)(disk_size / bytes_per_sector);

    /* Page alignment keeps DMA run boundaries aligned with frames. */
    u8 *ram_raw = kmalloc(disk_size + PAGE_SIZE);
    if (!ram_raw) {
        kfree(bpb_buf);
        return -1;
    }
    u8 *ram_disk =
        (u8 *)(((uintptr_t)ram_raw + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1));

    /* Read the image without crossing physical gaps. */
    u32 sectors_read = 0;
    while (sectors_read < total_sectors) {
        u8 *dst = ram_disk + (usize)sectors_read * bytes_per_sector;
        u32 left = total_sectors - sectors_read;
        u32 want = left < FAT_AHCI_CHUNK ? left : FAT_AHCI_CHUNK;

        u32 chunk = dma_run_sectors(dst, want, bytes_per_sector);
        u64 buf_phys = dma_phys(dst);
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
    usize image_size = 0;
    u32 bytes_per_sector = 0;
    u8 *image = fat_image_base(&image_size, &bytes_per_sector);
    if (!image || !bytes_per_sector || !g_ahci_dev || !ahci_backed ||
        !fat_write_through_enabled()) return;

    u32 total_sectors = (u32)(image_size / bytes_per_sector);
    u32 sectors_written = 0;

    while (sectors_written < total_sectors) {
        u8 *src = image + (usize)sectors_written * bytes_per_sector;
        u32 left = total_sectors - sectors_written;
        u32 want = left < FAT_AHCI_CHUNK ? left : FAT_AHCI_CHUNK;

        /* Restrict the request to the current physically contiguous run. */
        u32 chunk = dma_run_sectors(src, want, bytes_per_sector);
        u64 buf_phys = dma_phys(src);

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

int fat_read_sector(u32 lba, void *buf) {
    if (!g_ahci_dev) return -1;

    void *dma_buf = kmalloc(512);
    if (!dma_buf) return -1;
    u64 buf_phys = dma_phys(dma_buf);
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
