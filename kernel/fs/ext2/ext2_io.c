/* Device-backed image cache and explicit dirty-block writeback. The disk is
 * marked unclean BEFORE changing blocks, and clean only after a cache barrier.
 * This detects interrupted writes; it is not a journal or atomic rollback. */
#include "ext2_internal.h"
#include "ext2.h"
#include <fs/probe.h>
#include <memory/heap.h>
#include <utilities/string.h>

#define EXT2_CACHE_LIMIT (128u * 1024u * 1024u)
#define EXT2_VALID_FS 1u
#define EXT2_READ_CHUNK 128u

int ext2_mount_block(struct vfs_superblock *super, void *context) {
    const struct block_device *device = context;
    u8 header[2048];
    if (!device || !device->write || !device->flush ||
        block_read(device, 0, sizeof(header) / BLOCK_SECTOR_SIZE, header) ||
        !fs_probe_is_ext(header, sizeof(header))) return -1;
    struct ext2_superblock *disk = (void *)(header + EXT_SUPERBLOCK_OFFSET);
    if (disk->log_block_size > 2 || disk->state != EXT2_VALID_FS) return -1;
    u64 size = (u64)disk->blocks_count * (1024u << disk->log_block_size);
    if (size < sizeof(header) || size > EXT2_CACHE_LIMIT ||
        size / BLOCK_SECTOR_SIZE > device->sectors) return -1;
    u8 *image = kmalloc((usize)size);
    if (!image) return -1;
    u32 sectors = (u32)(size / BLOCK_SECTOR_SIZE);
    for (u32 lba = 0; lba < sectors;) {
        u32 chunk = sectors - lba < EXT2_READ_CHUNK ? sectors - lba : EXT2_READ_CHUNK;
        if (block_read(device, lba, chunk, image + (usize)lba * BLOCK_SECTOR_SIZE)) {
            kfree(image);
            return -1;
        }
        lba += chunk;
    }
    int result = ext2_mount_image(super, image, (usize)size);
    struct ext2_fs *fs = super->private_data;
    if (!fs) { kfree(image); return -1; }
    fs->owns_image = 1; /* Failed mount cleanup now owns the image as well. */
    if (result) return result;
    fs->device = *device;
    usize bitmap_size = ((usize)fs->superblock->blocks_count + 7) / 8;
    fs->dirty_blocks = kmalloc(bitmap_size);
    if (!fs->dirty_blocks) return -1;
    memset(fs->dirty_blocks, 0, bitmap_size);
    return 0;
}

int ext2_mount_device(const char *path, const struct block_device *device) {
    return vfs_attach(path, "ext2", (void *)device);
}

void ext2_dirty(struct ext2_fs *fs, const void *address, usize length) {
    if (!fs || !fs->dirty_blocks || !address || !length) return;
    uintptr_t base = (uintptr_t)fs->image, at = (uintptr_t)address;
    if (at < base || at - base >= fs->image_size || length > fs->image_size - (at - base)) {
        fs->io_failed = 1;
        return;
    }
    u32 first = (u32)((at - base) / fs->block_size);
    u32 last = (u32)((at - base + length - 1) / fs->block_size);
    for (u32 block = first; block <= last; block++)
        fs->dirty_blocks[block / 8] |= (u8)(1u << (block % 8));
}

static int write_block(struct ext2_fs *fs, u32 number) {
    u32 sectors = fs->block_size / BLOCK_SECTOR_SIZE;
    return block_write(&fs->device, (u64)number * sectors, sectors,
                        fs->image + (usize)number * fs->block_size);
}

int ext2_sync(struct ext2_fs *fs) {
    if (!fs) return -1;
    if (!fs->dirty_blocks) return 0;
    u32 blocks = fs->superblock->blocks_count;
    int dirty = 0;
    for (u32 i = 0; i < (blocks + 7) / 8; i++) dirty |= fs->dirty_blocks[i];
    if (!dirty) return fs->io_failed ? -1 : 0;
    u32 super_block = EXT_SUPERBLOCK_OFFSET / fs->block_size;
    fs->superblock->state &= ~EXT2_VALID_FS;
    if (write_block(fs, super_block) || block_flush(&fs->device)) goto failed;
    for (u32 number = 0; number < blocks; number++)
        if ((fs->dirty_blocks[number / 8] & (1u << (number % 8))) && write_block(fs, number))
            goto failed;
    if (block_flush(&fs->device)) goto failed;
    fs->superblock->state |= EXT2_VALID_FS;
    if (write_block(fs, super_block) || block_flush(&fs->device)) goto failed;
    memset(fs->dirty_blocks, 0, ((usize)blocks + 7) / 8);
    fs->io_failed = 0;
    return 0;
failed:
    fs->superblock->state &= ~EXT2_VALID_FS;
    fs->io_failed = 1; /* Keep dirty information; reject subsequent mutation. */
    return -1;
}

int ext2_sync_super(struct vfs_superblock *super) {
    return ext2_sync(super->private_data);
}
