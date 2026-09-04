/* kernel/fs/fat/fat_block.c , FAT on a block device.
 *
 * Reads a FAT volume off any transport into the RAM image the FAT engine
 * works on, and writes changed sectors back through that same device. This is
 * the shape ext2 already uses (kernel/fs/ext2/ext2_io.c): the block layer owns
 * DMA and addressing, so nothing here knows about physical addresses,
 * controller ports, or which transport it is talking to.
 *
 * FAT retains one mounted volume, so the device is a single static record
 * rather than per-superblock state. It must outlive the mount. A device mount
 * owns its RAM image; the plain image mount does not, and frees nothing.
 *
 * Implementation notes: block_device speaks fixed 512-byte sectors while a FAT
 * volume may use 512..4096, so LBAs coming out of the FAT engine are scaled on
 * the way down.
 */
#include <drivers/storage/block.h>
#include <fs/fat/fat.h>
#include <fs/fat/fat_internal.h>
#include <fs/fat/fat_vfs.h>
#include <fs/vfs/vfs.h>
#include <memory/heap.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* Sectors per request. The block layer bounces through one low frame, so this
 * is a throughput knob and not a DMA constraint. */
#define FAT_BLOCK_CHUNK 64u

static struct block_device fat_device;
static int fat_device_valid;
static u32 fat_sector_ratio; /* 512-byte sectors per FAT sector */
static u8 *fat_owned_image;

/* Write-through: called by the FAT engine for every sector it changes. */
static void device_sector_writer(u32 lba, const void *data) {
    if (!fat_device_valid || !fat_sector_ratio)
        return;
    if (block_write(&fat_device, (u64)lba * fat_sector_ratio, fat_sector_ratio,
                    data) != 0)
        log_write("FAT: sector write-through failed", FILESYS, LOG_ERROR);
}

int fat_mount_block(struct vfs_superblock *super, void *context) {
    const struct block_device *device = context;
    u8 boot[BLOCK_SECTOR_SIZE];
    if (!device || !device->write || !device->flush ||
        block_read(device, 0, 1, boot) != 0)
        return -1;

    u32 bytes_per_sector = 0;
    usize size = fat_volume_size(boot, &bytes_per_sector);
    if (!size || !bytes_per_sector || bytes_per_sector % BLOCK_SECTOR_SIZE ||
        size / BLOCK_SECTOR_SIZE > device->sectors)
        return -1;

    u8 *image = kmalloc(size);
    if (!image)
        return -1;

    u32 sectors = (u32)(size / BLOCK_SECTOR_SIZE);
    for (u32 lba = 0; lba < sectors;) {
        u32 left = sectors - lba;
        u32 chunk = left < FAT_BLOCK_CHUNK ? left : FAT_BLOCK_CHUNK;
        if (block_read(device, lba, chunk,
                       image + (usize)lba * BLOCK_SECTOR_SIZE) != 0) {
            kfree(image);
            return -1;
        }
        lba += chunk;
    }

    if (fat_vfs_mount_image(super, image, size) != 0) {
        kfree(image);
        return -1;
    }

    /* Publish the transport only after the volume is known good, so a failed
     * mount cannot leave a writer pointed at a device we never validated. */
    fat_device = *device;
    fat_device_valid = 1;
    fat_sector_ratio = bytes_per_sector / BLOCK_SECTOR_SIZE;
    fat_owned_image = image;
    fat_set_sector_writer(device_sector_writer);
    return 0;
}

int fat_mount_device(const char *mountpoint, const struct block_device *device) {
    return vfs_attach(mountpoint, FAT_VFS_NAME, (void *)device);
}

int fat_block_sync(struct vfs_superblock *super) {
    (void)super;
    if (!fat_device_valid)
        return 0;

    usize image_size = 0;
    u32 bytes_per_sector = 0;
    u8 *image = fat_image_base(&image_size, &bytes_per_sector);
    if (!image || !image_size)
        return 0;

    /* Every mutation already wrote its own sectors through
     * device_sector_writer, so this sweep is belt and braces against a write
     * path that forgot to. The flush after it is what actually commits any of
     * it past the transport's cache. */
    u32 sectors = (u32)(image_size / BLOCK_SECTOR_SIZE);
    for (u32 lba = 0; lba < sectors;) {
        u32 left = sectors - lba;
        u32 chunk = left < FAT_BLOCK_CHUNK ? left : FAT_BLOCK_CHUNK;
        if (block_write(&fat_device, lba, chunk,
                        image + (usize)lba * BLOCK_SECTOR_SIZE) != 0) {
            log_write("FAT: volume writeback failed", FILESYS, LOG_ERROR);
            return -1;
        }
        lba += chunk;
    }

    if (block_flush(&fat_device) != 0) {
        log_write("FAT: device flush failed", FILESYS, LOG_ERROR);
        return -1;
    }
    return 0;
}

void fat_block_release(void) {
    if (fat_owned_image)
        kfree(fat_owned_image);
    fat_owned_image = 0;
    fat_device_valid = 0;
    fat_sector_ratio = 0;
}
