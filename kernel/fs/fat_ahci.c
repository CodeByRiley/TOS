/* kernel/fs/fat_ahci.c — AHCI storage backend for the FAT driver.
 *
 * The volume is read into RAM whole at mount time and served from there;
 * writes go through to the disk a sector at a time via the write-through
 * hook, with fat_flush() as the bulk path. That is the same arrangement as
 * before this file existed — the only change is that the AHCI and
 * kernel-heap calls now live here instead of inside fat.c, so the
 * filesystem logic links without a disk driver behind it.
 */
#include "drivers/storage/ahci.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include <fs/fat.h>
#include <fs/fat_ahci.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* Sectors per AHCI request. 64 x 512 = 32 KiB, which is what the bulk
 * paths below have always moved per command. */
#define FAT_AHCI_CHUNK 64

extern struct AHCI_DEVICE_DATA *g_ahci_dev;

/* Set once a mount off this backend succeeds. Both write paths check it so
 * they stay inert when the image came from somewhere else — the Multiboot2
 * ramdisk fallback in main.c mounts an image the disk knows nothing about,
 * and pushing its sectors at a disk that failed to mount would overwrite
 * whatever is actually there. */
static int ahci_backed;

/* Write-through backend handed to fat.c. `data` points into the mounted
 * image, so the DMA address is just its physical translation. */
static void ahci_sector_writer(uint32_t lba, void *data) {
    if (!g_ahci_dev)
        return;

    uint64_t phys = virt_to_phys(data);
    ahci_write_sector(g_ahci_dev, 0, lba, 1, (void *)phys);
}

int fat_mount_from_ahci(struct AHCI_DEVICE_DATA *ahci_dev, int port) {
    if (!ahci_dev) return -1;

    // 1. Read the boot sector (LBA 0) to find out how big the disk is
    uint8_t *bpb_buf = kmalloc(512);
    if (!bpb_buf) return -1;

    uint64_t bpb_phys = virt_to_phys(bpb_buf);
    if (ahci_read_sector(ahci_dev, port, 0, 1, (void*)bpb_phys) != 0) {
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

    // 2. Allocate a RAM buffer big enough to hold the whole disk
    uint8_t *ram_disk = kmalloc(disk_size);
    if (!ram_disk) {
        kfree(bpb_buf);
        return -1;
    }

    // 3. Read the whole disk into RAM, 64 sectors (32KB) at a time
    uint32_t sectors_read = 0;
    while (sectors_read < total_sectors) {
        uint32_t chunk = FAT_AHCI_CHUNK;
        if (sectors_read + chunk > total_sectors) {
            chunk = total_sectors - sectors_read;
        }

        uint64_t buf_phys = virt_to_phys(ram_disk + (sectors_read * bytes_per_sector));
        if (ahci_read_sector(ahci_dev, port, sectors_read, chunk, (void*)buf_phys) != 0) {
            kfree(ram_disk);
            kfree(bpb_buf);
            return -1;
        }
        sectors_read += chunk;
    }

    // 4. Initialize your FAT driver with the RAM disk!
    int ret = fat_init(ram_disk, disk_size);

    /* Only arm the write-through once there is an image to write from;
     * arming it on a failed mount would push whatever fat_init rejected. */
    if (ret == 0) {
        ahci_backed = 1;
        fat_set_sector_writer(ahci_sector_writer);
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
        uint32_t chunk = FAT_AHCI_CHUNK;
        if (sectors_written + chunk > total_sectors) {
            chunk = total_sectors - sectors_written;
        }

        uint64_t buf_phys = virt_to_phys(image + (sectors_written * bytes_per_sector));

        if (ahci_write_sector(g_ahci_dev, 0, sectors_written, chunk, (void*)buf_phys) != 0) {
            log_write("FAT: Flush failed!", KERNEL, LOG_ERROR);
            return;
        }
        sectors_written += chunk;
    }
    log_write("FAT: Disk flushed to AHCI.", KERNEL, LOG_INFO);
}

int fat_read_sector(uint32_t lba, void *buf) {
    if (!g_ahci_dev) return -1;

    // Allocate a physical buffer for DMA
    void *dma_buf = kmalloc(512);
    if (!dma_buf) return -1;
    uint64_t dma_phys = virt_to_phys(dma_buf);

    // Read from AHCI Port 0 (where QEMU attached your disk.img)
    if (ahci_read_sector(g_ahci_dev, 0, lba, 1, (void*)dma_phys) != 0) {
        kfree(dma_buf);
        return -1;
    }

    // Copy the data from the DMA buffer into the FAT driver's buffer
    memcpy(buf, dma_buf, 512);
    kfree(dma_buf);
    return 0;
}
