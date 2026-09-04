/* FAT directory entries are inode identities; clusters are NOT identities
 * (empty files have no cluster, and truncation changes their first cluster). */
#include "fat_internal.h"
#include "fat_vfs.h"
#include <fs/vfs/vfs.h>
#include <utilities/string.h>

static const struct vfs_inode_operations inode_ops;
static const struct vfs_file_operations file_ops;
static struct vfs_superblock *mounted_super;

static u8 attributes(const struct vfs_inode *inode) {
    return inode->private_data ? ((const struct dir_entry *)inode->private_data)->attr
                               : FAT_ATTR_DIRECTORY;
}

static u64 entry_number(const void *entry) {
    return entry ? (u64)((const u8 *)entry - fat_volume.image) + 2 : 1;
}

static struct fat_dir directory(const struct vfs_inode *inode) {
    if (!inode->private_data) return root_directory();
    return (struct fat_dir){ .first_cluster = fat_impl_entry_get_cluster(inode->private_data) };
}

static struct vfs_inode *entry_inode(struct vfs_superblock *super, void *entry) {
    u8 attr = entry ? ((struct dir_entry *)entry)->attr : FAT_ATTR_DIRECTORY;
    return vfs_inode_get(super, entry_number(entry),
        attr & FAT_ATTR_DIRECTORY ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
        entry, &inode_ops, &file_ops);
}

static int lookup(struct vfs_inode *dir, const char *name, struct vfs_inode **out) {
    struct found_entry entry;
    if (fat_impl_find_entry(directory(dir), name, &entry)) return -1;
    *out = entry_inode(dir->super, entry.raw);
    return *out ? 0 : -1;
}

static int getattr(struct vfs_inode *inode, struct vfs_stat *out) {
    u8 attr = attributes(inode);
    u64 size = inode->private_data ? fat_impl_entry_get_size(inode->private_data) : 0;
    u32 mode = attr & FAT_ATTR_READ_ONLY ? 0444u : 0666u;
    mode |= inode->type == VFS_NODE_DIRECTORY ? 0040000u | 0111u : 0100000u;
    *out = (struct vfs_stat){ .inode = inode->number, .size = size,
        .blocks = (size + 511u) / 512u, .block_size = cluster_bytes(),
        .mode = mode, .type = inode->type, .attributes = attr };
    return 0;
}

static int create(struct vfs_inode *dir, const char *name, struct vfs_inode **out) {
    struct found_entry entry;
    struct fat_dir parent = directory(dir);
    if (fat_create_at(parent, name, &entry)) return -1;
    *out = entry_inode(dir->super, entry.raw);
    if (*out) return 0;
    fat_remove_at(parent, name, 0);
    return -1;
}

static int mkdir(struct vfs_inode *dir, const char *name) {
    return fat_mkdir_at(directory(dir), name);
}
static int unlink(struct vfs_inode *dir, const char *name) {
    return fat_remove_at(directory(dir), name, 0);
}
static int rmdir(struct vfs_inode *dir, const char *name) {
    return fat_remove_at(directory(dir), name, 1);
}

/* Rebuild the cluster cursor from current directory-entry metadata so
 * independent opens observe writes/truncation through the same inode. */
static int cursor(struct vfs_inode *inode, u64 position, struct fat_file *out) {
    if (!inode->private_data || position > UINT32_MAX) return -1;
    fat_file_from_entry(inode->private_data, out);
    return fat_seek(out, (u32)position);
}

static size_t read(struct vfs_file *file, void *buffer, size_t length) {
    struct fat_file fat;
    if (cursor(file->node, file->position, &fat)) return 0;
    size_t count = fat_read(&fat, buffer, length);
    file->position = fat.pos;
    return count;
}
static size_t write(struct vfs_file *file, const void *buffer, size_t length) {
    struct fat_file fat;
    if ((attributes(file->node) & FAT_ATTR_READ_ONLY) ||
        cursor(file->node, file->position, &fat)) return 0;
    if (length > UINT32_MAX - fat.pos) length = UINT32_MAX - fat.pos;
    size_t count = fat_write(&fat, buffer, length);
    file->position = fat.pos;
    return count;
}
static int seek(struct vfs_file *file, u64 position) {
    struct fat_file fat;
    if (cursor(file->node, position, &fat)) return -1;
    file->position = fat.pos;
    return 0;
}
static int truncate(struct vfs_inode *inode) {
    struct fat_file fat;
    if ((attributes(inode) & FAT_ATTR_READ_ONLY) || cursor(inode, 0, &fat)) return -1;
    return fat_truncate(&fat);
}
static long iterate(struct vfs_inode *dir, u32 *index, struct vfs_dirent *out) {
    int is_dir;
    struct fat_dir parent = directory(dir);
    long result = fat_impl_read_dir_one(parent, index, out->name, sizeof(out->name), &is_dir);
    if (result <= 0) return result;
    struct found_entry entry;
    if (fat_impl_find_entry(parent, out->name, &entry)) return -1;
    out->inode = entry_number(entry.raw);
    out->type = is_dir ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    return 1;
}

static const struct vfs_inode_operations inode_ops = {
    .lookup = lookup, .create = create, .mkdir = mkdir, .unlink = unlink,
    .rmdir = rmdir, .getattr = getattr, .truncate = truncate,
};
static const struct vfs_file_operations file_ops = {
    .read = read, .write = write, .seek = seek, .iterate = iterate,
};

static int probe(const void *image, size_t size) {
    if (!image || size < 512) return 0;
    size_t volume_size = fat_volume_size(image, 0);
    return volume_size && volume_size <= size;
}
static int publish_root(struct vfs_superblock *super) {
    if (mounted_super || fat_volume.type == FAT_TYPE_NONE) return -1;
    super->root = entry_inode(super, 0);
    if (!super->root) return -1;
    mounted_super = super;
    super->private_data = &fat_volume;
    return 0;
}
/* Mount a volume already sitting in memory. The caller owns that memory; a
 * plain image has nowhere to write back to, so write-through stays off. */
int fat_vfs_mount_image(struct vfs_superblock *super, void *image, usize size) {
    /* The retained FAT disk engine is single-volume. Reject a second mount
     * BEFORE fat_init can replace the state beneath existing handles. */
    if (mounted_super || fat_init(image, size)) return -1;
    fat_set_sector_writer(0);
    return publish_root(super);
}
static void unmount(struct vfs_superblock *super) {
    if (mounted_super == super) {
        mounted_super = 0;
        fat_set_sector_writer(0);
        fat_block_release();
    }
}
static const struct vfs_filesystem fat_filesystem = {
    .name = FAT_VFS_NAME, .probe = probe, .mount = fat_vfs_mount_image,
    .attach = fat_mount_block, .sync = fat_block_sync, .unmount = unmount,
};

void fat_vfs_register(void) { vfs_register(&fat_filesystem); }
