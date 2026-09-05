/* Shared VFS implementation details, not a second backend API. */
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H
#include "vfs.h"
#include "lock.h"
#include <utilities/errno.h>
#include <memory/heap.h>
#include <utilities/string.h>

struct vfs_mount {
    char path[VFS_PATH_MAX];
    struct vfs_superblock super;
};
/* Temporary name-to-inode association. The chain owns its inode references;
 * keeping parents makes '..' work across mounts without backend guesses. */
struct vfs_dentry {
    struct vfs_dentry *parent;
    struct vfs_inode *inode;
    const struct vfs_mount *mount;
    size_t path_length;
};
struct vfs_path {
    struct vfs_dentry *entry;
    char name[VFS_PATH_MAX];
};
int vfs_path_length(const char *path, size_t *length);
struct vfs_mount *vfs_find_mount(const char *path);
int vfs_mount_contains(const char *path);
int vfs_lookup(const char *path, struct vfs_path *out);
int vfs_lookup_parent(const char *path, struct vfs_path *parent,
                      char name[VFS_NAME_MAX + 1], int *trailing_slash);
void vfs_path_put(struct vfs_path *path);
int vfs_getattr(struct vfs_inode *inode, struct vfs_stat *out);
#endif
