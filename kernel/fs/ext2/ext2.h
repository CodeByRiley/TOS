/* kernel/fs/ext2/ext2.h , ext2 VFS backend registration.
 *
 * Exposes the ext2 filesystem type to kernel startup code. On-disk layouts
 * and backend helpers remain private to ext2_internal.h.
 *
 * Implementation: kernel/fs/ext2/ext2_vfs.c.
 */
#ifndef KERNEL_EXT2_H
#define KERNEL_EXT2_H

void ext2_vfs_register(void);

#endif
