/* Filesystem-neutral objects and API. See ../README.md for ownership rules. */
#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H
#include <stddef.h>
#include <stdint.h>

#define VFS_PATH_MAX 260
#define VFS_NAME_MAX 255
#define VFS_MAX_FILESYSTEMS 8
#define VFS_MAX_MOUNTS 8

enum vfs_node_type { VFS_NODE_NONE, VFS_NODE_FILE, VFS_NODE_DIRECTORY };
struct vfs_stat {
    uint64_t size, inode, blocks;
    uint32_t block_size, mode;
    uint8_t type, attributes;
};
struct vfs_dirent {
    uint64_t inode;
    uint8_t type;
    char name[VFS_NAME_MAX + 1];
};
struct vfs_inode;
struct vfs_file;
struct vfs_superblock;
struct vfs_mount;

/* Name operations receive ONE name in a known directory, never a path.
 * lookup/create return one owned inode reference; failures return none. */
struct vfs_inode_operations {
    int (*lookup)(struct vfs_inode *dir, const char *name, struct vfs_inode **out);
    int (*create)(struct vfs_inode *dir, const char *name, struct vfs_inode **out);
    int (*mkdir)(struct vfs_inode *dir, const char *name);
    int (*unlink)(struct vfs_inode *dir, const char *name);
    int (*rmdir)(struct vfs_inode *dir, const char *name);
    int (*getattr)(struct vfs_inode *inode, struct vfs_stat *out);
    int (*truncate)(struct vfs_inode *inode);
};
/* release must tolerate a partially failed open. iterate returns >0 for an
 * entry, 0 at EOF, -1 on failure; index is a backend-owned cookie. */
struct vfs_file_operations {
    int (*open)(struct vfs_file *file);
    void (*release)(struct vfs_file *file);
    size_t (*read)(struct vfs_file *file, void *buffer, size_t length);
    size_t (*write)(struct vfs_file *file, const void *buffer, size_t length);
    int (*seek)(struct vfs_file *file, uint64_t position);
    long (*iterate)(struct vfs_inode *dir, uint32_t *index, struct vfs_dirent *out);
};
/* Live inodes are shared by (superblock, number), not pathname.
 * private_data borrows backend/image state; it must outlive the inode. */
struct vfs_inode {
    struct vfs_superblock *super;
    uint64_t number;
    uint8_t type;
    void *private_data;
    const struct vfs_inode_operations *operations;
    const struct vfs_file_operations *file_operations;
    unsigned references;
    struct vfs_inode *next;
};
struct vfs_filesystem {
    const char *name;
    int (*probe)(const void *image, size_t size);
    int (*mount)(struct vfs_superblock *super, void *image, size_t size);
    int (*attach)(struct vfs_superblock *super, void *context);
    void (*unmount)(struct vfs_superblock *super);
    int (*sync)(struct vfs_superblock *super);
};
/* Mount supplies private_data and an owned root reference. unmount frees
 * backend state, never the caller-owned image; it also handles failed mounts. */
struct vfs_superblock {
    const struct vfs_filesystem *filesystem;
    void *private_data;
    struct vfs_inode *root, *inodes;
    unsigned open_files;
};
struct vfs_file {
    const struct vfs_mount *mount;
    struct vfs_inode *node;
    void *private_data;
    uint64_t position;
    /* Compatibility snapshots; vfs_file_stat refreshes from the inode. */
    uint64_t size, inode;
    uint8_t type, attributes;
};
/* Backend-only reference helpers: caller must already hold the VFS gate. */
struct vfs_inode *vfs_inode_get(struct vfs_superblock *super, uint64_t number,
    uint8_t type, void *private_data, const struct vfs_inode_operations *operations,
    const struct vfs_file_operations *file_operations);
struct vfs_inode *vfs_inode_ref(struct vfs_inode *inode);
void vfs_inode_put(struct vfs_inode *inode);

/* Public operations serialize internally and may sleep. BSP task context only
 * (bootstrap allowed); never call from an IRQ, AP, or backend callback.
 * Init is boot-time only. Unmount before releasing a mounted image.
 * Callers own handle/buffer storage and must keep it alive until calls return;
 * separate calls (e.g. seek + write) are not one atomic operation. */
void vfs_init(void);
int vfs_register(const struct vfs_filesystem *filesystem);
int vfs_mount_image(const char *mountpoint, const char *filesystem,
                    void *image, size_t size);
int vfs_mount_auto(const char *mountpoint, void *image, size_t size,
                   const char **mounted_type);
int vfs_attach(const char *mountpoint, const char *filesystem, void *context);
int vfs_unmount(const char *mountpoint);
int vfs_sync_all(void);
int vfs_file_sync(struct vfs_file *file);
int vfs_open(const char *path, struct vfs_file *file);
int vfs_create(const char *path, struct vfs_file *file);
void vfs_close(struct vfs_file *file);
size_t vfs_read(struct vfs_file *file, void *buffer, size_t length);
size_t vfs_write(struct vfs_file *file, const void *buffer, size_t length);
/* Refresh EOF, seek and write under one lock; ignores a stale size snapshot. */
size_t vfs_append(struct vfs_file *file, const void *buffer, size_t length);
int vfs_seek(struct vfs_file *file, uint64_t position);
int vfs_truncate(struct vfs_file *file);
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_file_stat(struct vfs_file *file, struct vfs_stat *out);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
long vfs_read_dir_one(const char *path, uint32_t *index, struct vfs_dirent *out);
long vfs_read_dir(const char *path, uint32_t *index, char *buffer, size_t length);
#endif
