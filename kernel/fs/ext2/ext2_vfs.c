/* ext2's component-level VFS adapter. Disk algorithms stay in dir/inode/file. */
#include "ext2_internal.h"
#include "ext2.h"
#include <utilities/string.h>

static const struct vfs_inode_operations inode_ops;
static const struct vfs_file_operations file_ops;

struct vfs_inode *ext2_vfs_inode(struct vfs_superblock *super, u32 number) {
    struct ext2_inode *disk = ext2_inode_get(super->private_data, number);
    if (!disk) return 0;
    u16 kind = disk->mode & EXT2_S_IFMT;
    if (kind != EXT2_S_IFREG && kind != EXT2_S_IFDIR) return 0;
    /* Do not silently corrupt attributes, indexed directories, immutable or
     * append-only files. Those inode semantics need dedicated implementations. */
    if (disk->file_acl || disk->fragment_address || (disk->flags & ~0x00c8u) ||
        (kind == EXT2_S_IFREG && disk->directory_acl)) return 0;
    return vfs_inode_get(super, number,
        kind == EXT2_S_IFDIR ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
        disk, &inode_ops, &file_ops);
}

static int lookup(struct vfs_inode *dir, const char *name, struct vfs_inode **out) {
    u32 number;
    if (ext2_dir_lookup(dir->super->private_data, dir->number, name, &number, 0))
        return -1;
    *out = ext2_vfs_inode(dir->super, number);
    return *out ? 0 : -1;
}

static int getattr(struct vfs_inode *inode, struct vfs_stat *out) {
    struct ext2_fs *fs = inode->super->private_data;
    struct ext2_inode *disk = inode->private_data;
    *out = (struct vfs_stat){ .inode = inode->number, .size = disk->size,
        .blocks = disk->sectors_count, .block_size = fs->block_size,
        .mode = disk->mode, .type = inode->type };
    return 0;
}

static int create(struct vfs_inode *dir, const char *name, struct vfs_inode **out) {
    struct ext2_fs *fs = dir->super->private_data;
    if (fs->io_failed) return -1;
    u32 number;
    if (!ext2_dir_lookup(fs, dir->number, name, 0, 0) ||
        ext2_inode_allocate(fs, EXT2_S_IFREG | 0644u, &number)) return -1;
    struct ext2_inode *disk = ext2_inode_get(fs, number);
    struct vfs_inode *inode = ext2_vfs_inode(dir->super, number);
    if (!inode || ext2_dir_add(fs, dir->number, name, number, EXT2_FT_REG_FILE)) {
        vfs_inode_put(inode);
        if (disk) ext2_inode_free(fs, number, disk);
        return -1;
    }
    if (ext2_sync(fs)) { vfs_inode_put(inode); return -1; }
    *out = inode;
    return 0;
}

static int unlink(struct vfs_inode *dir, const char *name) {
    struct ext2_fs *fs = dir->super->private_data;
    if (fs->io_failed) return -1;
    u32 number;
    if (ext2_dir_lookup(fs, dir->number, name, &number, 0)) return -1;
    struct ext2_inode *disk = ext2_inode_get(fs, number);
    if (!disk || (disk->mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
        ext2_dir_remove(fs, dir->number, name, 0, 0)) return -1;
    ext2_dirty(fs, disk, fs->inode_size);
    if (disk->links_count > 1) disk->links_count--;
    else ext2_inode_free(fs, number, disk);
    return ext2_sync(fs);
}

static void dot_entry(struct ext2_fs *fs, struct ext2_dir_entry *entry, u32 number, u16 length,
                       const char *name, u8 name_length) {
    memset(entry, 0, length);
    entry->inode = number;
    entry->record_length = length;
    entry->name_length = name_length;
    entry->file_type = ext2_directory_type(fs, EXT2_FT_DIR);
    memcpy(entry->name, name, name_length);
}

static int mkdir(struct vfs_inode *dir, const char *name) {
    struct ext2_fs *fs = dir->super->private_data;
    if (fs->io_failed) return -1;
    u32 number;
    if (!ext2_dir_lookup(fs, dir->number, name, 0, 0) ||
        ext2_inode_allocate(fs, EXT2_S_IFDIR | 0755u, &number)) return -1;
    struct ext2_inode *disk = ext2_inode_get(fs, number);
    u32 block_number = disk ? ext2_inode_block(fs, disk, 0, 1) : 0;
    u8 *block = block_number ? ext2_block(fs, block_number) : 0;
    if (!block) goto fail;
    disk->links_count = 2;
    disk->size = fs->block_size;
    dot_entry(fs, (struct ext2_dir_entry *)block, number, 12, ".", 1);
    dot_entry(fs, (struct ext2_dir_entry *)(block + 12), dir->number,
              (u16)(fs->block_size - 12), "..", 2);
    if (ext2_dir_add(fs, dir->number, name, number, EXT2_FT_DIR)) goto fail;
    ((struct ext2_inode *)dir->private_data)->links_count++;
    ext2_dirty(fs, dir->private_data, fs->inode_size);
    return ext2_sync(fs);
fail:
    if (disk) ext2_inode_free(fs, number, disk);
    return -1;
}

static int rmdir(struct vfs_inode *dir, const char *name) {
    struct ext2_fs *fs = dir->super->private_data;
    if (fs->io_failed) return -1;
    u32 number;
    if (ext2_dir_lookup(fs, dir->number, name, &number, 0) ||
        number == EXT2_ROOT_INODE) return -1;
    struct ext2_inode *disk = ext2_inode_get(fs, number);
    if (!disk || (disk->mode & EXT2_S_IFMT) != EXT2_S_IFDIR ||
        !ext2_dir_empty(fs, number) || ext2_dir_remove(fs, dir->number, name, 0, 0))
        return -1;
    struct ext2_inode *parent = dir->private_data;
    if (parent->links_count) parent->links_count--;
    ext2_dirty(fs, parent, fs->inode_size);
    ext2_inode_free(fs, number, disk);
    return ext2_sync(fs);
}

static int truncate(struct vfs_inode *inode) {
    struct ext2_fs *fs = inode->super->private_data;
    if (fs->io_failed) return -1;
    ext2_inode_truncate(fs, inode->private_data);
    return ext2_sync(fs);
}

static size_t read(struct vfs_file *file, void *buffer, size_t length) {
    return ext2_file_read(file->node->super->private_data, file->node->private_data,
                          &file->position, buffer, length);
}

static size_t write(struct vfs_file *file, const void *buffer, size_t length) {
    struct ext2_fs *fs = file->node->super->private_data;
    if (fs->io_failed) return 0;
    size_t count = ext2_file_write(fs, file->node->private_data, &file->position, buffer, length);
    return ext2_sync(fs) ? 0 : count;
}

static int seek(struct vfs_file *file, u64 position) {
    if (position > UINT32_MAX) return -1;
    file->position = position;
    return 0;
}

static long iterate(struct vfs_inode *dir, u32 *index, struct vfs_dirent *out) {
    return ext2_dir_read_one(dir->super->private_data, dir->number, index, out);
}

static const struct vfs_inode_operations inode_ops = {
    .lookup = lookup, .create = create, .mkdir = mkdir, .unlink = unlink,
    .rmdir = rmdir, .getattr = getattr, .truncate = truncate,
};
static const struct vfs_file_operations file_ops = {
    .read = read, .write = write, .seek = seek, .iterate = iterate,
};

void ext2_vfs_register(void) {
    extern const struct vfs_filesystem ext2_filesystem;
    vfs_register(&ext2_filesystem);
}
