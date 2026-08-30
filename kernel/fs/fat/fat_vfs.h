#ifndef TOS_FAT_VFS_H
#define TOS_FAT_VFS_H

void fat_vfs_register(void);
int fat_vfs_attach(const char *mountpoint);

#endif
