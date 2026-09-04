/* kernel/fs/fat/fat_vfs.h , FAT VFS backend registration.
 *
 * Exposes the FAT filesystem type to kernel startup code. On-disk layouts and
 * backend helpers remain private to fat_internal.h.
 *
 * Implementation: kernel/fs/fat/fat_vfs.c, kernel/fs/fat/fat_block.c.
 */
#ifndef KERNEL_FAT_VFS_H
#define KERNEL_FAT_VFS_H

#define FAT_VFS_NAME "fat"

void fat_vfs_register(void);
struct block_device;
/* Device/context must outlive the mount. The filesystem owns its RAM cache. */
int fat_mount_device(const char *mountpoint, const struct block_device *device);

#endif
