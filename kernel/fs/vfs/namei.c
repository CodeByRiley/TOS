/* Component lookup and namespace mutations. No disk-format knowledge. */
#include "internal.h"

int vfs_path_length(const char *path, size_t *length) {
    if (!path || path[0] != '/') return -1;
    for (size_t i = 1; i < VFS_PATH_MAX; i++) {
        if (!path[i]) { *length = i; return 0; }
    }
    return -1;
}

static void pop_entry(struct vfs_path *path) {
    struct vfs_dentry *entry = path->entry;
    path->entry = entry->parent;
    vfs_inode_put(entry->inode);
    kfree(entry);
    path->name[path->entry ? path->entry->path_length : 0] = 0;
}

void vfs_path_put(struct vfs_path *path) {
    while (path->entry) pop_entry(path);
}

/* Consumes inode even on allocation failure. */
static int push_entry(struct vfs_path *path, struct vfs_inode *inode,
                       const struct vfs_mount *mount) {
    struct vfs_dentry *entry = kmalloc(sizeof(*entry));
    if (!entry) { vfs_inode_put(inode); return -1; }
    *entry = (struct vfs_dentry){ .parent = path->entry, .inode = inode,
        .mount = mount, .path_length = strlen(path->name) };
    path->entry = entry;
    return 0;
}

int vfs_lookup(const char *path, struct vfs_path *out) {
    memset(out, 0, sizeof(*out));
    size_t length;
    struct vfs_mount *root = vfs_find_mount("/");
    if (!root || vfs_path_length(path, &length)) return -1;
    strcpy(out->name, "/");
    if (push_entry(out, vfs_inode_ref(root->super.root), root)) return -1;
    for (size_t i = 1; i < length;) {
        if (path[i] == '/') { i++; continue; }
        struct vfs_inode *dir = out->entry->inode;
        if (dir->type != VFS_NODE_DIRECTORY) goto fail;
        size_t start = i;
        while (i < length && path[i] != '/') i++;
        size_t count = i - start;
        if (count > VFS_NAME_MAX) goto fail;
        char name[VFS_NAME_MAX + 1];
        memcpy(name, path + start, count);
        name[count] = 0;
        if (!strcmp(name, ".")) continue;
        if (!strcmp(name, "..")) {
            if (out->entry->parent) pop_entry(out);
            continue;
        }
        size_t at = out->entry->path_length;
        if (at > 1) out->name[at++] = '/';
        if (count >= sizeof(out->name) - at) goto fail;
        memcpy(out->name + at, name, count + 1);
        const struct vfs_mount *mount = vfs_find_mount(out->name);
        struct vfs_inode *child = 0;
        if (mount) {
            child = vfs_inode_ref(mount->super.root);
        } else {
            mount = out->entry->mount;
            if (!dir->operations->lookup || dir->operations->lookup(dir, name, &child))
                goto fail;
        }
        if (!child || push_entry(out, child, mount)) goto fail;
    }
    if (path[length - 1] == '/' && out->entry->inode->type != VFS_NODE_DIRECTORY)
        goto fail;
    return 0;
fail:
    vfs_path_put(out);
    return -1;
}

int vfs_lookup_parent(const char *path, struct vfs_path *parent,
                      char name[VFS_NAME_MAX + 1], int *trailing_slash) {
    size_t length;
    memset(parent, 0, sizeof(*parent));
    if (vfs_path_length(path, &length)) return -1;
    *trailing_slash = path[length - 1] == '/';
    while (length > 1 && path[length - 1] == '/') length--;
    size_t start = length;
    while (start && path[start - 1] != '/') start--;
    size_t count = length - start;
    if (!count || count > VFS_NAME_MAX) return -1;
    memcpy(name, path + start, count);
    name[count] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -1;
    char prefix[VFS_PATH_MAX];
    memcpy(prefix, path, start);
    prefix[start] = 0;
    if (vfs_lookup(prefix, parent)) return -1;
    if (parent->entry->inode->type != VFS_NODE_DIRECTORY) goto fail;
    size_t at = strlen(parent->name);
    if (at > 1) parent->name[at++] = '/';
    if (count >= sizeof(parent->name) - at) goto fail;
    memcpy(parent->name + at, name, count + 1);
    /* Mutating a mountpoint (or its ancestor) would orphan a mount. */
    if (vfs_mount_contains(parent->name)) goto fail;
    return 0;
fail:
    vfs_path_put(parent);
    return -1;
}

int vfs_getattr(struct vfs_inode *inode, struct vfs_stat *out) {
    if (!inode || !out || !inode->operations->getattr) return -1;
    memset(out, 0, sizeof(*out));
    return inode->operations->getattr(inode, out);
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    struct vfs_path found;
    if (!out || vfs_lookup(path, &found)) return -1;
    int result = vfs_getattr(found.entry->inode, out);
    vfs_path_put(&found);
    return result;
}

enum name_operation { CREATE_DIRECTORY, REMOVE_FILE, REMOVE_DIRECTORY };

static int change_name(const char *path, enum name_operation operation) {
    struct vfs_path parent;
    char name[VFS_NAME_MAX + 1];
    int trailing;
    if (vfs_lookup_parent(path, &parent, name, &trailing)) return -1;
    struct vfs_inode *dir = parent.entry->inode;
    const struct vfs_inode_operations *ops = dir->operations;
    struct vfs_inode *target = 0;
    int result = -1;
    if (operation == CREATE_DIRECTORY) {
        if (ops->mkdir) result = ops->mkdir(dir, name);
    } else if (ops->lookup && !ops->lookup(dir, name, &target)) {
        /* Defer POSIX unlink-open semantics until backends support orphaned
         * storage. Never let an open handle point at freed/reused disk data. */
        if (target->references == 1) {
            if (operation == REMOVE_FILE && !trailing &&
                target->type == VFS_NODE_FILE && ops->unlink)
                result = ops->unlink(dir, name);
            if (operation == REMOVE_DIRECTORY &&
                target->type == VFS_NODE_DIRECTORY && ops->rmdir)
                result = ops->rmdir(dir, name);
        }
    }
    vfs_inode_put(target);
    vfs_path_put(&parent);
    return result;
}

int vfs_mkdir(const char *path) { return change_name(path, CREATE_DIRECTORY); }
int vfs_unlink(const char *path) { return change_name(path, REMOVE_FILE); }
int vfs_rmdir(const char *path) { return change_name(path, REMOVE_DIRECTORY); }
