/* kernel/fs/ext2/ext2_internal.h , private ext2 layouts and helpers.
 *
 * Defines the supported ext2 on-disk structures, mount state, open-file
 * state, and helpers shared by the split ext2 implementation. Files outside
 * kernel/fs/ext2 include ext2.h or vfs.h instead.
 *
 * Implementation: kernel/fs/ext2/ext2_mount.c,
 *                 kernel/fs/ext2/ext2_inode.c,
 *                 kernel/fs/ext2/ext2_dir.c,
 *                 kernel/fs/ext2/ext2_file.c,
 *                 kernel/fs/ext2/ext2_vfs.c.
 */
#ifndef KERNEL_EXT2_INTERNAL_H
#define KERNEL_EXT2_INTERNAL_H

#include <utilities/types.h>
#include <fs/vfs/vfs.h>
#include <stddef.h>
#include <stdint.h>

#define EXT2_ROOT_INODE 2u
#define EXT2_NDIR_BLOCKS 12u
#define EXT2_IND_BLOCK 12u
#define EXT2_DIND_BLOCK 13u
#define EXT2_TIND_BLOCK 14u

#define EXT2_S_IFMT 0170000u
#define EXT2_S_IFREG 0100000u
#define EXT2_S_IFDIR 0040000u

#define EXT2_FT_UNKNOWN 0u
#define EXT2_FT_REG_FILE 1u
#define EXT2_FT_DIR 2u

#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u

struct PACKED ext2_superblock {
    u32 inodes_count;
    u32 blocks_count;
    u32 reserved_blocks_count;
    u32 free_blocks_count;
    u32 free_inodes_count;
    u32 first_data_block;
    u32 log_block_size;
    i32 log_fragment_size;
    u32 blocks_per_group;
    u32 fragments_per_group;
    u32 inodes_per_group;
    u32 mount_time;
    u32 write_time;
    u16 mount_count;
    i16 max_mount_count;
    u16 magic;
    u16 state;
    u16 errors;
    u16 minor_revision;
    u32 last_check;
    u32 check_interval;
    u32 creator_os;
    u32 revision_level;
    u16 default_reserved_uid;
    u16 default_reserved_gid;
    u32 first_inode;
    u16 inode_size;
    u16 block_group_number;
    u32 feature_compat;
    u32 feature_incompat;
    u32 feature_ro_compat;
};

struct PACKED ext2_group_descriptor {
    u32 block_bitmap;
    u32 inode_bitmap;
    u32 inode_table;
    u16 free_blocks_count;
    u16 free_inodes_count;
    u16 used_directories_count;
    u16 pad;
    u8 reserved[12];
};

struct PACKED ext2_inode {
    u16 mode;
    u16 uid;
    u32 size;
    u32 access_time;
    u32 creation_time;
    u32 modification_time;
    u32 deletion_time;
    u16 gid;
    u16 links_count;
    u32 sectors_count;
    u32 flags;
    u32 os_specific_1;
    u32 block[15];
    u32 generation;
    u32 file_acl;
    u32 directory_acl;
    u32 fragment_address;
    u8 os_specific_2[12];
};

struct PACKED ext2_dir_entry {
    u32 inode;
    u16 record_length;
    u8 name_length;
    u8 file_type;
    char name[];
};

struct ext2_fs {
    u8 *image;
    usize image_size;
    struct ext2_superblock *superblock;
    struct ext2_group_descriptor *groups;
    u32 block_size;
    u32 inode_size;
    u32 group_count;
    u32 pointers_per_block;
};

struct ext2_open_file {
    u32 inode_number;
};

void *ext2_bytes(struct ext2_fs *fs, u64 offset, usize length);
void *ext2_block(struct ext2_fs *fs, u32 block_number);
struct ext2_inode *ext2_inode_get(struct ext2_fs *fs, u32 inode_number);
int ext2_inode_allocate(struct ext2_fs *fs, u16 mode,
                        u32 *inode_number_out);
void ext2_inode_free(struct ext2_fs *fs, u32 inode_number,
                     struct ext2_inode *inode);
u32 ext2_block_allocate(struct ext2_fs *fs,
                             struct ext2_inode *owner);
void ext2_block_free(struct ext2_fs *fs, u32 block_number,
                     struct ext2_inode *owner);
u32 ext2_inode_block(struct ext2_fs *fs, struct ext2_inode *inode,
                          u32 logical_block, int create);
void ext2_inode_truncate(struct ext2_fs *fs, struct ext2_inode *inode);

int ext2_path_resolve(struct ext2_fs *fs, const char *path,
                      u32 *inode_number_out);
int ext2_path_parent(struct ext2_fs *fs, const char *path,
                     u32 *parent_inode_out, char leaf[VFS_NAME_MAX + 1]);
int ext2_dir_lookup(struct ext2_fs *fs, u32 directory_inode,
                    const char *name, u32 *inode_number_out,
                    u8 *type_out);
int ext2_dir_add(struct ext2_fs *fs, u32 directory_inode,
                 const char *name, u32 inode_number, u8 type);
int ext2_dir_remove(struct ext2_fs *fs, u32 directory_inode,
                    const char *name, u32 *inode_number_out,
                    u8 *type_out);
int ext2_dir_empty(struct ext2_fs *fs, u32 directory_inode);
long ext2_dir_read_one(struct ext2_fs *fs, u32 directory_inode,
                       u32 *index, struct vfs_dirent *out);

usize ext2_file_read(struct ext2_fs *fs, struct ext2_inode *inode,
                      u64 *position, void *buffer, usize length);
usize ext2_file_write(struct ext2_fs *fs, struct ext2_inode *inode,
                       u64 *position, const void *buffer, usize length);

#endif
