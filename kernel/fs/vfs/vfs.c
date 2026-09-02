/* Filesystem registration, mount ownership and live inode references. */
#include "internal.h"

static const struct vfs_filesystem *filesystems[VFS_MAX_FILESYSTEMS];
static struct vfs_mount mounts[VFS_MAX_MOUNTS];

void vfs_init(void) {
    /* Reinitialization must not invalidate live handles. */
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].super.filesystem) return;
    memset(filesystems, 0, sizeof(filesystems));
    memset(mounts, 0, sizeof(mounts));
}

static const struct vfs_filesystem *find_type(const char *name) {
    if (name)
        for (size_t i = 0; i < VFS_MAX_FILESYSTEMS; i++)
            if (filesystems[i] && !strcmp(filesystems[i]->name, name))
                return filesystems[i];
    return 0;
}

int vfs_register(const struct vfs_filesystem *type) {
    if (!type || !type->name || !*type->name || !type->mount || find_type(type->name))
        return -1;
    for (size_t i = 0; i < VFS_MAX_FILESYSTEMS; i++) {
        if (!filesystems[i]) {
            filesystems[i] = type;
            return 0;
        }
    }
    return -1;
}

struct vfs_mount *vfs_find_mount(const char *path) {
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].super.filesystem && !strcmp(mounts[i].path, path))
            return &mounts[i];
    return 0;
}

/* True for a mountpoint or an ancestor containing a mount. */
int vfs_mount_contains(const char *path) {
    size_t length = strlen(path);
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        const char *mounted = mounts[i].path;
        if (mounts[i].super.filesystem && !strncmp(mounted, path, length) &&
            (length == 1 || !mounted[length] || mounted[length] == '/'))
            return 1;
    }
    return 0;
}

/* Normalize slashes but reject dot components in mount names. */
static int mount_name(const char *path, char out[VFS_PATH_MAX]) {
    size_t length;
    if (vfs_path_length(path, &length)) return -1;
    size_t at = 0;
    out[at++] = '/';
    for (size_t i = 1; i < length;) {
        if (path[i] == '/') { i++; continue; }
        size_t start = i;
        while (i < length && path[i] != '/') i++;
        size_t count = i - start;
        if (count > VFS_NAME_MAX ||
            (count == 1 && path[start] == '.') ||
            (count == 2 && path[start] == '.' && path[start + 1] == '.'))
            return -1;
        if (at > 1) out[at++] = '/';
        memcpy(out + at, path + start, count);
        at += count;
    }
    out[at] = 0;
    return 0;
}

static void release_super(struct vfs_superblock *super) {
    vfs_inode_put(super->root);
    super->root = 0;
    if (super->filesystem->unmount) super->filesystem->unmount(super);
    memset(super, 0, sizeof(*super));
}

static int mount_type(const char *path, const struct vfs_filesystem *type,
                      void *source, size_t size, int attach) {
    char name[VFS_PATH_MAX];
    if (!type || !source || mount_name(path, name) || vfs_find_mount(name))
        return -1;
    /* Validate capacity BEFORE calling a backend with mutable mount state. */
    struct vfs_mount *mount = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        if (!mounts[i].super.filesystem) { mount = &mounts[i]; break; }
    if (!mount || (attach && !type->attach) ||
        (!attach && (!size || (type->probe && type->probe(source, size) <= 0))))
        return -1;
    struct vfs_superblock *super = &mount->super;
    super->filesystem = type;
    int result = attach ? type->attach(super, source) : type->mount(super, source, size);
    if (result || !super->root || super->root->type != VFS_NODE_DIRECTORY) {
        release_super(super);
        return -1;
    }
    strcpy(mount->path, name);
    return 0;
}

int vfs_mount_image(const char *path, const char *type, void *image, size_t size) {
    return mount_type(path, find_type(type), image, size, 0);
}

int vfs_attach(const char *path, const char *type, void *context) {
    return mount_type(path, find_type(type), context, 0, 1);
}

int vfs_mount_auto(const char *path, void *image, size_t size, const char **type) {
    if (type) *type = 0;
    for (size_t i = 0; i < VFS_MAX_FILESYSTEMS; i++) {
        if (filesystems[i] && !mount_type(path, filesystems[i], image, size, 0)) {
            if (type) *type = filesystems[i]->name;
            return 0;
        }
    }
    return -1;
}

int vfs_unmount(const char *path) {
    char name[VFS_PATH_MAX];
    if (mount_name(path, name)) return -1;
    struct vfs_mount *mount = vfs_find_mount(name);
    if (!mount || mount->super.open_files) return -1;
    size_t length = strlen(name);
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
        if (&mounts[i] != mount && mounts[i].super.filesystem &&
            !strncmp(mounts[i].path, name, length) &&
            (length == 1 || mounts[i].path[length] == '/'))
            return -1;
    for (struct vfs_inode *inode = mount->super.inodes; inode; inode = inode->next)
        if (inode != mount->super.root || inode->references != 1) return -1;
    if (mount->super.filesystem->sync && mount->super.filesystem->sync(&mount->super))
        return -1;
    release_super(&mount->super);
    memset(mount, 0, sizeof(*mount));
    return 0;
}

struct vfs_inode *vfs_inode_ref(struct vfs_inode *inode) {
    if (inode) inode->references++;
    return inode;
}

int vfs_sync_all(void) {
    int result = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        struct vfs_superblock *super = &mounts[i].super;
        if (super->filesystem && super->filesystem->sync && super->filesystem->sync(super))
            result = -1;
    }
    return result;
}

int vfs_file_sync(struct vfs_file *file) {
    if (!file || !file->node) return -1;
    struct vfs_superblock *super = file->node->super;
    return super->filesystem->sync ? super->filesystem->sync(super) : 0;
}

struct vfs_inode *vfs_inode_get(struct vfs_superblock *super, uint64_t number,
    uint8_t type, void *data, const struct vfs_inode_operations *ops,
    const struct vfs_file_operations *file_ops) {
    for (struct vfs_inode *inode = super->inodes; inode; inode = inode->next)
        if (inode->number == number) return vfs_inode_ref(inode);
    struct vfs_inode *inode = kmalloc(sizeof(*inode));
    if (!inode) return 0;
    *inode = (struct vfs_inode){ .super = super, .number = number, .type = type,
        .private_data = data, .operations = ops, .file_operations = file_ops,
        .references = 1, .next = super->inodes };
    super->inodes = inode;
    return inode;
}

void vfs_inode_put(struct vfs_inode *inode) {
    if (!inode || --inode->references) return;
    struct vfs_inode **link = &inode->super->inodes;
    while (*link != inode) link = &(*link)->next;
    *link = inode->next;
    kfree(inode);
}
