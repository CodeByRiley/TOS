/* Filesystem-neutral path, file, metadata, and mount dispatch. */
#ifndef TOS_VFS_H
#define TOS_VFS_H

#include <utilities/types.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_PATH_MAX 260
#define VFS_NAME_MAX 255
#define VFS_MAX_FILESYSTEMS 8
#define VFS_MAX_MOUNTS 8

enum vfs_node_type {
    VFS_NODE_NONE = 0,
    VFS_NODE_FILE = 1,
    VFS_NODE_DIRECTORY = 2,
};

struct vfs_stat {
    u64 size;
    u64 inode;
    u64 blocks;
    u32 block_size;
    u32 mode;
    u8 type;
    u8 attributes;
};

struct vfs_dirent {
    u64 inode;
    u8 type;
    char name[VFS_NAME_MAX + 1];
};

struct vfs_file;

struct vfs_operations {
    int (*open)(void *fs, const char *path, struct vfs_file *file);
    int (*create)(void *fs, const char *path, struct vfs_file *file);
    void (*close)(void *fs, struct vfs_file *file);
    usize (*read)(void *fs, struct vfs_file *file, void *buffer,
                   usize length);
    usize (*write)(void *fs, struct vfs_file *file, const void *buffer,
                    usize length);
    int (*seek)(void *fs, struct vfs_file *file, u64 position);
    int (*truncate)(void *fs, struct vfs_file *file);
    int (*stat)(void *fs, const char *path, struct vfs_stat *out);
    int (*file_stat)(void *fs, struct vfs_file *file, struct vfs_stat *out);
    int (*unlink)(void *fs, const char *path);
    int (*mkdir)(void *fs, const char *path);
    int (*rmdir)(void *fs, const char *path);
    long (*read_dir)(void *fs, const char *path, u32 *index,
                     struct vfs_dirent *out);
};

struct vfs_filesystem {
    const char *name;
    int (*probe)(const void *image, usize size);
    int (*mount)(void *image, usize size, void **fs_out);
    void (*unmount)(void *fs);
    const struct vfs_operations *operations;
};

struct vfs_mount;

struct vfs_file {
    const struct vfs_mount *mount;
    void *private_data;
    u64 position;
    u64 size;
    u64 inode;
    u8 type;
    u8 attributes;
};

void vfs_init(void);
int vfs_register(const struct vfs_filesystem *filesystem);
int vfs_mount_image(const char *mountpoint, const char *filesystem,
                    void *image, usize size);
int vfs_mount_auto(const char *mountpoint, void *image, usize size,
                   const char **mounted_type);
int vfs_attach(const char *mountpoint, const char *filesystem, void *fs);

int vfs_open(const char *path, struct vfs_file *file);
int vfs_create(const char *path, struct vfs_file *file);
void vfs_close(struct vfs_file *file);
usize vfs_read(struct vfs_file *file, void *buffer, usize length);
usize vfs_write(struct vfs_file *file, const void *buffer, usize length);
int vfs_seek(struct vfs_file *file, u64 position);
int vfs_truncate(struct vfs_file *file);
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_file_stat(struct vfs_file *file, struct vfs_stat *out);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
long vfs_read_dir_one(const char *path, u32 *index,
                      struct vfs_dirent *out);
long vfs_read_dir(const char *path, u32 *index, char *buffer,
                  usize length);

#endif
