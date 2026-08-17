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
#include "memory/vmm.h"
#include <fs/fat.h>
#include <fs/fat_ahci.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* Upper bound on sectors per AHCI request. 64 x 512 = 32 KiB. A request may
 * be cut shorter than this — see dma_run_sectors. */
#define FAT_AHCI_CHUNK 64
#define PAGE_SIZE 4096u

extern struct AHCI_DEVICE_DATA *g_ahci_dev;

/* Physical address of a kernel heap pointer, for handing to a DMA engine.
 *
 * NOT virt_to_phys(): that is a flat HHDM_BASE subtraction valid only inside
 * the direct map, and hhdm.h says so explicitly. The heap lives at
 * HEAP_BASE (0xFFFF9000_00000000), one terabyte-range above HHDM_BASE
 * (0xFFFF8000_00000000), so subtracting yielded ~16 TiB — a physical address
 * no real machine has. Every DMA through this file was aimed there, which is
 * why mounting from a real disk always failed and fell back to the ramdisk.
 *
 * vmm_translate walks the page tables and returns 0 when unmapped. */
static uint64_t dma_phys(const void *virt) {
    return vmm_translate((uint64_t)(uintptr_t)virt);
}

/* How many sectors starting at `base` can go in one DMA command.
 *
 * kmalloc is contiguous in virtual memory only: heap_grow() maps one
 * independently allocated PMM frame per page, so a 32 KiB transfer issued
 * against a single physical base address would write the first page
 * correctly and then run 28 KiB into whatever physical memory happens to
 * follow it. Walk forward while the frames stay consecutive and stop at the
 * first discontinuity.
 *
 * `base` must be page-aligned so each step lands on a page boundary. */
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

    /* One sector never straddles a page, so no run-length check is needed —
     * only the translation has to be right. */
    uint64_t phys = dma_phys(data);
    if (!phys)
        return;
    ahci_write_sector(g_ahci_dev, 0, lba, 1, (void *)phys);
}

int fat_mount_from_ahci(struct AHCI_DEVICE_DATA *ahci_dev, int port) {
    if (!ahci_dev) return -1;

    // 1. Read the boot sector (LBA 0) to find out how big the disk is
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

    /* 2. Allocate the RAM image, page-aligned.
     *
     * Over-allocate by a page and round up rather than trusting kmalloc's
     * 8-byte alignment: dma_run_sectors steps a page at a time, so a base
     * starting mid-page would make every run boundary land inside a page and
     * the sector arithmetic would stop lining up with the frames. */
    uint8_t *ram_raw = kmalloc(disk_size + PAGE_SIZE);
    if (!ram_raw) {
        kfree(bpb_buf);
        return -1;
    }
    uint8_t *ram_disk =
        (uint8_t *)(((uintptr_t)ram_raw + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1));

    /* 3. Read the whole disk into RAM, at most 32 KiB per command and never
     *    across a physical discontinuity in the heap mapping. */
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

    // 4. Initialize your FAT driver with the RAM disk!
    int ret = fat_init(ram_disk, disk_size);

    /* Only arm the write-through once there is an image to write from;
     * arming it on a failed mount would push whatever fat_init rejected. */
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

        /* Same page-run limit as the read path. Writing across a physical
         * discontinuity would send unrelated memory to the disk — the read
         * bug only corrupts RAM, this one corrupts the volume. */
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

    // Allocate a bounce buffer for DMA
    void *dma_buf = kmalloc(512);
    if (!dma_buf) return -1;
    uint64_t buf_phys = dma_phys(dma_buf);
    if (!buf_phys) {
        kfree(dma_buf);
        return -1;
    }

    // Read from AHCI Port 0 (where QEMU attached your disk.img)
    if (ahci_read_sector(g_ahci_dev, 0, lba, 1, (void*)buf_phys) != 0) {
        kfree(dma_buf);
        return -1;
    }

    // Copy the data from the DMA buffer into the FAT driver's buffer
    memcpy(buf, dma_buf, 512);
    kfree(dma_buf);
    return 0;
}
