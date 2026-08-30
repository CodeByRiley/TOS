#include <fs/ext2/ext2_internal.h>
#include <utilities/string.h>

usize ext2_file_read(struct ext2_fs *fs, struct ext2_inode *inode,
                      u64 *position, void *buffer, usize length) {
    if (!fs || !inode || !position || !buffer || *position >= inode->size)
        return 0;
    u64 available = inode->size - *position;
    if (length > available)
        length = (usize)available;
    u8 *out = buffer;
    usize total = 0;
    while (total < length) {
        u32 logical = (u32)(*position / fs->block_size);
        u32 offset = (u32)(*position % fs->block_size);
        usize chunk = fs->block_size - offset;
        if (chunk > length - total)
            chunk = length - total;
        u32 number = ext2_inode_block(fs, inode, logical, 0);
        void *block = number ? ext2_block(fs, number) : 0;
        if (block)
            memcpy(out + total, (u8 *)block + offset, chunk);
        else
            memset(out + total, 0, chunk);
        *position += chunk;
        total += chunk;
    }
    return total;
}

usize ext2_file_write(struct ext2_fs *fs, struct ext2_inode *inode,
                       u64 *position, const void *buffer, usize length) {
    if (!fs || !inode || !position || !buffer || *position > UINT32_MAX)
        return 0;
    const u8 *in = buffer;
    usize total = 0;
    while (total < length && *position <= UINT32_MAX) {
        u32 logical = (u32)(*position / fs->block_size);
        u32 offset = (u32)(*position % fs->block_size);
        usize chunk = fs->block_size - offset;
        if (chunk > length - total)
            chunk = length - total;
        if (chunk > UINT32_MAX - *position)
            chunk = (usize)(UINT32_MAX - *position);
        if (!chunk)
            break;
        u32 number = ext2_inode_block(fs, inode, logical, 1);
        u8 *block = ext2_block(fs, number);
        if (!block)
            break;
        memcpy(block + offset, in + total, chunk);
        *position += chunk;
        total += chunk;
        if (*position > inode->size)
            inode->size = (u32)*position;
    }
    return total;
}
