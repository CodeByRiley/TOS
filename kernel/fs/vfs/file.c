/* Open-file lifetime, byte transfers and directory enumeration. */
#include "internal.h"

static void close_locked(struct vfs_file *file);

static int file_stat_locked(struct vfs_file *file, struct vfs_stat *out) {
    if (!file || vfs_getattr(file->node, out)) return -1;
    file->size = out->size;
    file->inode = out->inode;
    file->type = out->type;
    file->attributes = out->attributes;
    return 0;
}

static void refresh_file(struct vfs_file *file) {
    struct vfs_stat metadata;
    file_stat_locked(file, &metadata);
}

int vfs_file_stat(struct vfs_file *file, struct vfs_stat *out) {
    VFS_GUARD();
    return file_stat_locked(file, out);
}

/* Consumes an inode reference, including on a failed open. */
static int open_inode(struct vfs_inode *inode, const struct vfs_mount *mount,
                       struct vfs_file *file) {
    file->node = inode;
    file->mount = mount;
    inode->super->open_files++;
    const struct vfs_file_operations *ops = inode->file_operations;
    struct vfs_stat metadata;
    if ((ops && ops->open && ops->open(file)) || file_stat_locked(file, &metadata)) {
        close_locked(file);
        return -1;
    }
    return 0;
}

int vfs_open(const char *path, struct vfs_file *file) {
    VFS_GUARD();
    if (!file) return -1;
    memset(file, 0, sizeof(*file));
    struct vfs_path found;
    if (vfs_lookup(path, &found)) return -1;
    int result = open_inode(vfs_inode_ref(found.entry->inode), found.entry->mount, file);
    vfs_path_put(&found);
    return result;
}

int vfs_create(const char *path, struct vfs_file *file) {
    VFS_GUARD();
    if (!file) return -1;
    memset(file, 0, sizeof(*file));
    struct vfs_path parent;
    char name[VFS_NAME_MAX + 1];
    int trailing;
    if (vfs_lookup_parent(path, &parent, name, &trailing)) return -1;
    struct vfs_inode *dir = parent.entry->inode;
    struct vfs_inode *inode = 0;
    int result = -1;
    if (!trailing && dir->operations->create &&
        !dir->operations->create(dir, name, &inode)) {
        result = open_inode(inode, parent.entry->mount, file);
        /* create's directory entry is new: roll it back if opening fails. */
        if (result && dir->operations->unlink) dir->operations->unlink(dir, name);
    }
    vfs_path_put(&parent);
    return result;
}

static void close_locked(struct vfs_file *file) {
    if (!file) return;
    if (file->node) {
        const struct vfs_file_operations *ops = file->node->file_operations;
        if (ops && ops->release) ops->release(file);
        file->node->super->open_files--;
        vfs_inode_put(file->node);
    }
    memset(file, 0, sizeof(*file));
}

void vfs_close(struct vfs_file *file) {
    VFS_GUARD();
    close_locked(file);
}

static const struct vfs_file_operations *regular_ops(struct vfs_file *file) {
    return file && file->node && file->node->type == VFS_NODE_FILE
        ? file->node->file_operations : 0;
}

size_t vfs_read(struct vfs_file *file, void *buffer, size_t length) {
    VFS_GUARD();
    const struct vfs_file_operations *ops = regular_ops(file);
    if (!ops || !ops->read || !buffer || !length) return 0;
    size_t result = ops->read(file, buffer, length);
    refresh_file(file);
    return result;
}

static size_t write_locked(struct vfs_file *file, const void *buffer, size_t length) {
    const struct vfs_file_operations *ops = regular_ops(file);
    if (!ops || !ops->write || !buffer || !length) return 0;
    size_t result = ops->write(file, buffer, length);
    refresh_file(file);
    return result;
}

size_t vfs_write(struct vfs_file *file, const void *buffer, size_t length) {
    VFS_GUARD();
    return write_locked(file, buffer, length);
}

size_t vfs_append(struct vfs_file *file, const void *buffer, size_t length) {
    VFS_GUARD();
    const struct vfs_file_operations *ops = regular_ops(file);
    struct vfs_stat metadata;
    if (!buffer || !length || !ops || !ops->write || !ops->seek ||
        file_stat_locked(file, &metadata) || ops->seek(file, metadata.size)) return 0;
    return write_locked(file, buffer, length);
}

int vfs_seek(struct vfs_file *file, uint64_t position) {
    VFS_GUARD();
    const struct vfs_file_operations *ops = regular_ops(file);
    if (!ops || !ops->seek) return -1;
    int result = ops->seek(file, position);
    refresh_file(file);
    return result;
}

int vfs_truncate(struct vfs_file *file) {
    VFS_GUARD();
    if (!regular_ops(file) || !file->node->operations->truncate) return -1;
    int result = file->node->operations->truncate(file->node);
    if (!result) file->position = 0;
    refresh_file(file);
    return result;
}

static long iterate(struct vfs_inode *dir, uint32_t *index, struct vfs_dirent *out) {
    const struct vfs_file_operations *ops = dir->file_operations;
    if (dir->type != VFS_NODE_DIRECTORY || !ops || !ops->iterate) return -1;
    memset(out, 0, sizeof(*out));
    return ops->iterate(dir, index, out);
}

long vfs_read_dir_one(const char *path, uint32_t *index, struct vfs_dirent *out) {
    VFS_GUARD();
    struct vfs_path found;
    if (!index || !out || vfs_lookup(path, &found)) return -1;
    uint32_t before = *index;
    long result = iterate(found.entry->inode, index, out);
    if (result < 0) *index = before;
    vfs_path_put(&found);
    return result;
}

long vfs_read_dir(const char *path, uint32_t *index, char *buffer, size_t length) {
    VFS_GUARD();
    struct vfs_path found;
    if (!index || !buffer || !length || vfs_lookup(path, &found)) return -1;
    size_t written = 0;
    long result;
    for (;;) {
        uint32_t before = *index;
        struct vfs_dirent entry;
        long status = iterate(found.entry->inode, index, &entry);
        if (status <= 0) {
            if (status < 0) *index = before;
            result = status < 0 && !written ? -1 : (long)written;
            break;
        }
        size_t count = strlen(entry.name);
        size_t required = count + 1 + (entry.type == VFS_NODE_DIRECTORY);
        if (required > length - written) {
            *index = before;
            result = written ? (long)written : -1;
            break;
        }
        memcpy(buffer + written, entry.name, count);
        written += count;
        if (entry.type == VFS_NODE_DIRECTORY) buffer[written++] = '/';
        buffer[written++] = 0;
    }
    vfs_path_put(&found);
    return result;
}
