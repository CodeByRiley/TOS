/* kernel/fs/vfs/vfs.h , virtual-filesystem interface.
 *
 * Defines filesystem registration, mountpoint routing, generic file handles,
 * metadata, and directory entries. Backends provide a vfs_operations table;
 * callers remain independent of the mounted disk format.
 *
 * Implementation: kernel/fs/vfs/vfs.c.
 */
#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

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
    uint64_t size;
    uint64_t inode;
    uint64_t blocks;
    uint32_t block_size;
    uint32_t mode;
    uint8_t type;
    uint8_t attributes;
};

struct vfs_dirent {
    uint64_t inode;
    uint8_t type;
    char name[VFS_NAME_MAX + 1];
};

struct vfs_file;

struct vfs_operations {
    int (*open)(void *fs, const char *path, struct vfs_file *file);
    int (*create)(void *fs, const char *path, struct vfs_file *file);
    void (*close)(void *fs, struct vfs_file *file);
    size_t (*read)(void *fs, struct vfs_file *file, void *buffer,
                   size_t length);
    size_t (*write)(void *fs, struct vfs_file *file, const void *buffer,
                    size_t length);
    int (*seek)(void *fs, struct vfs_file *file, uint64_t position);
    int (*truncate)(void *fs, struct vfs_file *file);
    int (*stat)(void *fs, const char *path, struct vfs_stat *out);
    int (*file_stat)(void *fs, struct vfs_file *file, struct vfs_stat *out);
    int (*unlink)(void *fs, const char *path);
    int (*mkdir)(void *fs, const char *path);
    int (*rmdir)(void *fs, const char *path);
    long (*read_dir)(void *fs, const char *path, uint32_t *index,
                     struct vfs_dirent *out);
};

struct vfs_filesystem {
    const char *name;
    int (*probe)(const void *image, size_t size);
    int (*mount)(void *image, size_t size, void **fs_out);
    void (*unmount)(void *fs);
    const struct vfs_operations *operations;
};

struct vfs_mount;

struct vfs_file {
    const struct vfs_mount *mount;
    void *private_data;
    uint64_t position;
    uint64_t size;
    uint64_t inode;
    uint8_t type;
    uint8_t attributes;
};

void vfs_init(void);
int vfs_register(const struct vfs_filesystem *filesystem);
int vfs_mount_image(const char *mountpoint, const char *filesystem,
                    void *image, size_t size);
int vfs_mount_auto(const char *mountpoint, void *image, size_t size,
                   const char **mounted_type);
int vfs_attach(const char *mountpoint, const char *filesystem, void *fs);

int vfs_open(const char *path, struct vfs_file *file);
int vfs_create(const char *path, struct vfs_file *file);
void vfs_close(struct vfs_file *file);
size_t vfs_read(struct vfs_file *file, void *buffer, size_t length);
size_t vfs_write(struct vfs_file *file, const void *buffer, size_t length);
int vfs_seek(struct vfs_file *file, uint64_t position);
int vfs_truncate(struct vfs_file *file);
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_file_stat(struct vfs_file *file, struct vfs_stat *out);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
long vfs_read_dir_one(const char *path, uint32_t *index,
                      struct vfs_dirent *out);
long vfs_read_dir(const char *path, uint32_t *index, char *buffer,
                  size_t length);

#endif
