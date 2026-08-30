/* kernel/fs/fat/fat_vfs.h , FAT-to-VFS adapter registration.
 *
 * Registers the FAT filesystem type and attaches an already initialized FAT
 * image to a VFS mountpoint.
 *
 * Implementation: kernel/fs/fat/fat_vfs.c.
 */
#ifndef KERNEL_FAT_VFS_H
#define KERNEL_FAT_VFS_H

#define FAT_VFS_NAME "fat"

void fat_vfs_register(void);
int fat_vfs_attach(const char *mountpoint);

#endif
