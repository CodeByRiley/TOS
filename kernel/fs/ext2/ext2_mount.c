#include <fs/ext2/ext2_internal.h>
#include <memory/heap.h>
#include <utilities/string.h>

void *ext2_bytes(struct ext2_fs *fs, u64 offset, usize length) {
    if (!fs || offset > fs->image_size || length > fs->image_size - offset)
        return 0;
    return fs->image + (usize)offset;
}

void *ext2_block(struct ext2_fs *fs, u32 block_number) {
    if (!fs || block_number >= fs->superblock->blocks_count)
        return 0;
    return ext2_bytes(fs, (u64)block_number * fs->block_size,
                      fs->block_size);
}

static int ext2_probe_image(const void *image, usize size) {
    if (!image || size < 2048)
        return 0;
    const u8 *bytes = image;
    const struct ext2_superblock *superblock =
        (const struct ext2_superblock *)(bytes + 1024);
    return superblock->magic == EXT2_MAGIC;
}

static int ext2_mount_image(void *image, usize size, void **fs_out) {
    if (!fs_out || !ext2_probe_image(image, size))
        return -1;
    struct ext2_superblock *superblock =
        (struct ext2_superblock *)((u8 *)image + 1024);
    if (superblock->log_block_size > 2 ||
        superblock->blocks_per_group == 0 ||
        superblock->inodes_per_group == 0 ||
        superblock->blocks_count <= superblock->first_data_block ||
        (superblock->feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) ||
        (superblock->feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE))
        return -1;

    u32 block_size = 1024u << superblock->log_block_size;
    if (superblock->blocks_per_group > block_size * 8u ||
        superblock->inodes_per_group > block_size * 8u)
        return -1;
    u64 volume_size = (u64)superblock->blocks_count * block_size;
    if (volume_size > size)
        return -1;
    u32 data_blocks = superblock->blocks_count -
                           superblock->first_data_block;
    u32 group_count =
        (data_blocks + superblock->blocks_per_group - 1) /
        superblock->blocks_per_group;
    u32 inode_groups =
        (superblock->inodes_count + superblock->inodes_per_group - 1) /
        superblock->inodes_per_group;
    if (group_count == 0 || inode_groups != group_count)
        return -1;

    u32 inode_size = superblock->revision_level == 0
                              ? 128u
                              : superblock->inode_size;
    if (inode_size < sizeof(struct ext2_inode) || inode_size > block_size ||
        (inode_size & 3u))
        return -1;

    u64 descriptor_offset =
        (u64)(superblock->first_data_block + 1) * block_size;
    usize descriptor_bytes =
        (usize)group_count * sizeof(struct ext2_group_descriptor);
    struct ext2_group_descriptor *groups =
        descriptor_offset <= size && descriptor_bytes <= size - descriptor_offset
            ? (struct ext2_group_descriptor *)((u8 *)image +
                                                descriptor_offset)
            : 0;
    if (!groups)
        return -1;

    struct ext2_fs *fs = kmalloc(sizeof(*fs));
    if (!fs)
        return -1;
    memset(fs, 0, sizeof(*fs));
    fs->image = image;
    fs->image_size = size;
    fs->superblock = superblock;
    fs->groups = groups;
    fs->block_size = block_size;
    fs->inode_size = inode_size;
    fs->group_count = group_count;
    fs->pointers_per_block = block_size / sizeof(u32);

    u32 inode_table_blocks =
        (u32)(((u64)superblock->inodes_per_group * inode_size +
                    block_size - 1) /
                   block_size);
    for (u32 group = 0; group < group_count; group++) {
        const struct ext2_group_descriptor *descriptor = &groups[group];
        if (!ext2_block(fs, descriptor->block_bitmap) ||
            !ext2_block(fs, descriptor->inode_bitmap) ||
            descriptor->inode_table >= superblock->blocks_count ||
            inode_table_blocks > superblock->blocks_count -
                                     descriptor->inode_table) {
            kfree(fs);
            return -1;
        }
    }

    struct ext2_inode *root = ext2_inode_get(fs, EXT2_ROOT_INODE);
    if (!root || (root->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        kfree(fs);
        return -1;
    }
    *fs_out = fs;
    return 0;
}

static void ext2_unmount_image(void *context) {
    if (context)
        kfree(context);
}

extern const struct vfs_operations ext2_vfs_operations;

const struct vfs_filesystem ext2_filesystem = {
    .name = "ext2",
    .probe = ext2_probe_image,
    .mount = ext2_mount_image,
    .unmount = ext2_unmount_image,
    .operations = &ext2_vfs_operations,
};
