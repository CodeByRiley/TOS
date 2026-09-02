#include <fs/ext2/ext2_internal.h>
#include <utilities/string.h>

static int bitmap_test(const u8 *bitmap, u32 bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1u;
}

static void bitmap_set(u8 *bitmap, u32 bit) {
    bitmap[bit / 8] |= (u8)(1u << (bit % 8));
}

static void bitmap_clear(u8 *bitmap, u32 bit) {
    bitmap[bit / 8] &= (u8)~(1u << (bit % 8));
}

struct ext2_inode *ext2_inode_get(struct ext2_fs *fs,
                                  u32 inode_number) {
    if (!fs || inode_number == 0 ||
        inode_number > fs->superblock->inodes_count)
        return 0;
    u32 index = inode_number - 1;
    u32 group = index / fs->superblock->inodes_per_group;
    u32 in_group = index % fs->superblock->inodes_per_group;
    if (group >= fs->group_count)
        return 0;
    u64 offset =
        (u64)fs->groups[group].inode_table * fs->block_size +
        (u64)in_group * fs->inode_size;
    return ext2_bytes(fs, offset, fs->inode_size);
}

int ext2_inode_allocate(struct ext2_fs *fs, u16 mode,
                        u32 *inode_number_out) {
    if (!fs || !inode_number_out)
        return -1;
    u32 first = fs->superblock->revision_level == 0
                         ? 11u
                         : fs->superblock->first_inode;
    if (first < 11)
        first = 11;

    for (u32 group = 0; group < fs->group_count; group++) {
        struct ext2_group_descriptor *descriptor = &fs->groups[group];
        if (!descriptor->free_inodes_count)
            continue;
        u8 *bitmap = ext2_block(fs, descriptor->inode_bitmap);
        if (!bitmap)
            return -1;
        u32 group_first = group * fs->superblock->inodes_per_group + 1;
        u32 count = fs->superblock->inodes_per_group;
        if (group_first + count - 1 > fs->superblock->inodes_count)
            count = fs->superblock->inodes_count - group_first + 1;
        for (u32 bit = 0; bit < count; bit++) {
            u32 number = group_first + bit;
            if (number < first || bitmap_test(bitmap, bit))
                continue;
            struct ext2_inode *inode = ext2_inode_get(fs, number);
            if (!inode)
                return -1;
            bitmap_set(bitmap, bit);
            descriptor->free_inodes_count--;
            fs->superblock->free_inodes_count--;
            memset(inode, 0, fs->inode_size);
            inode->mode = mode;
            inode->links_count = 1;
            if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
                descriptor->used_directories_count++;
            ext2_dirty_group(fs, group, bitmap);
            ext2_dirty(fs, inode, fs->inode_size);
            *inode_number_out = number;
            return 0;
        }
    }
    return -1;
}

u32 ext2_block_allocate(struct ext2_fs *fs,
                             struct ext2_inode *owner) {
    if (!fs)
        return 0;
    for (u32 group = 0; group < fs->group_count; group++) {
        struct ext2_group_descriptor *descriptor = &fs->groups[group];
        if (!descriptor->free_blocks_count)
            continue;
        u8 *bitmap = ext2_block(fs, descriptor->block_bitmap);
        if (!bitmap)
            return 0;
        u32 group_first = fs->superblock->first_data_block +
                               group * fs->superblock->blocks_per_group;
        u32 count = fs->superblock->blocks_per_group;
        if (group_first + count > fs->superblock->blocks_count)
            count = fs->superblock->blocks_count - group_first;
        for (u32 bit = 0; bit < count; bit++) {
            if (bitmap_test(bitmap, bit))
                continue;
            u32 number = group_first + bit;
            void *block = ext2_block(fs, number);
            if (!block)
                return 0;
            bitmap_set(bitmap, bit);
            descriptor->free_blocks_count--;
            fs->superblock->free_blocks_count--;
            memset(block, 0, fs->block_size);
            if (owner)
                owner->sectors_count += fs->block_size / 512u;
            ext2_dirty_group(fs, group, bitmap);
            ext2_dirty(fs, block, fs->block_size);
            ext2_dirty(fs, owner, fs->inode_size);
            return number;
        }
    }
    return 0;
}

void ext2_block_free(struct ext2_fs *fs, u32 block_number,
                     struct ext2_inode *owner) {
    if (!fs || block_number < fs->superblock->first_data_block ||
        block_number >= fs->superblock->blocks_count)
        return;
    u32 relative = block_number - fs->superblock->first_data_block;
    u32 group = relative / fs->superblock->blocks_per_group;
    u32 bit = relative % fs->superblock->blocks_per_group;
    if (group >= fs->group_count)
        return;
    u8 *bitmap = ext2_block(fs, fs->groups[group].block_bitmap);
    if (!bitmap || !bitmap_test(bitmap, bit))
        return;
    bitmap_clear(bitmap, bit);
    fs->groups[group].free_blocks_count++;
    fs->superblock->free_blocks_count++;
    void *block = ext2_block(fs, block_number);
    if (block)
        memset(block, 0, fs->block_size);
    u32 sectors = fs->block_size / 512u;
    if (owner && owner->sectors_count >= sectors)
        owner->sectors_count -= sectors;
    ext2_dirty_group(fs, group, bitmap);
    ext2_dirty(fs, block, fs->block_size);
    ext2_dirty(fs, owner, fs->inode_size);
}

static u32 ensure_pointer_block(struct ext2_fs *fs,
                                     struct ext2_inode *inode,
                                     void *pointer, int create) {
    u32 number;
    memcpy(&number, pointer, sizeof(number));
    if (number)
        return number;
    if (!create)
        return 0;
    number = ext2_block_allocate(fs, inode);
    memcpy(pointer, &number, sizeof(number));
    ext2_dirty(fs, pointer, sizeof(number));
    return number;
}

u32 ext2_inode_block(struct ext2_fs *fs, struct ext2_inode *inode,
                          u32 logical_block, int create) {
    if (!fs || !inode)
        return 0;
    u8 *roots = (u8 *)inode + offsetof(struct ext2_inode, block);
    if (logical_block < EXT2_NDIR_BLOCKS)
        return ensure_pointer_block(fs, inode, roots + logical_block * sizeof(u32), create);

    /* Single, double and triple indirection are the same tree walk. Each
     * pointer at a level covers 'stride' logical data blocks. */
    u64 index = logical_block - EXT2_NDIR_BLOCKS;
    u64 capacity = fs->pointers_per_block;
    for (int depth = 1; depth <= 3; depth++, capacity *= fs->pointers_per_block) {
        if (index >= capacity) { index -= capacity; continue; }
        void *pointer = roots + (EXT2_IND_BLOCK + depth - 1) * sizeof(u32);
        u64 stride = capacity;
        for (int level = depth; level > 0; level--) {
            u32 number = ensure_pointer_block(fs, inode, pointer, create);
            u32 *entries = ext2_block(fs, number);
            if (!entries) return 0;
            stride /= fs->pointers_per_block;
            pointer = &entries[index / stride];
            index %= stride;
        }
        return ensure_pointer_block(fs, inode, pointer, create);
    }
    return 0;
}

static void free_indirect(struct ext2_fs *fs, struct ext2_inode *inode,
                          u32 block_number, int depth) {
    u32 *pointers = ext2_block(fs, block_number);
    if (!pointers)
        return;
    for (u32 i = 0; i < fs->pointers_per_block; i++) {
        if (!pointers[i])
            continue;
        if (depth > 1)
            free_indirect(fs, inode, pointers[i], depth - 1);
        else
            ext2_block_free(fs, pointers[i], inode);
    }
    ext2_block_free(fs, block_number, inode);
}

void ext2_inode_truncate(struct ext2_fs *fs, struct ext2_inode *inode) {
    if (!fs || !inode)
        return;
    for (u32 i = 0; i < EXT2_NDIR_BLOCKS; i++)
        if (inode->block[i])
            ext2_block_free(fs, inode->block[i], inode);
    if (inode->block[EXT2_IND_BLOCK])
        free_indirect(fs, inode, inode->block[EXT2_IND_BLOCK], 1);
    if (inode->block[EXT2_DIND_BLOCK])
        free_indirect(fs, inode, inode->block[EXT2_DIND_BLOCK], 2);
    if (inode->block[EXT2_TIND_BLOCK])
        free_indirect(fs, inode, inode->block[EXT2_TIND_BLOCK], 3);
    memset(inode->block, 0, sizeof(inode->block));
    inode->size = 0;
    inode->directory_acl = 0;
    inode->sectors_count = 0;
    ext2_dirty(fs, inode, fs->inode_size);
}

void ext2_inode_free(struct ext2_fs *fs, u32 inode_number,
                     struct ext2_inode *inode) {
    if (!fs || !inode || inode_number == 0 ||
        inode_number > fs->superblock->inodes_count)
        return;
    u16 old_mode = inode->mode;
    ext2_inode_truncate(fs, inode);
    u32 index = inode_number - 1;
    u32 group = index / fs->superblock->inodes_per_group;
    u32 bit = index % fs->superblock->inodes_per_group;
    u8 *bitmap = ext2_block(fs, fs->groups[group].inode_bitmap);
    if (bitmap && bitmap_test(bitmap, bit)) {
        bitmap_clear(bitmap, bit);
        fs->groups[group].free_inodes_count++;
        fs->superblock->free_inodes_count++;
        if ((old_mode & EXT2_S_IFMT) == EXT2_S_IFDIR &&
            fs->groups[group].used_directories_count)
            fs->groups[group].used_directories_count--;
        ext2_dirty_group(fs, group, bitmap);
    }
    memset(inode, 0, fs->inode_size);
    ext2_dirty(fs, inode, fs->inode_size);
}
