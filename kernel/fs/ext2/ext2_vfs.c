#include <fs/ext2/ext2.h>
#include <fs/ext2/ext2_internal.h>
#include <memory/heap.h>
#include <utilities/string.h>

static u8 vfs_type(const struct ext2_inode *inode) {
    return (inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR
               ? VFS_NODE_DIRECTORY
               : VFS_NODE_FILE;
}

static void fill_stat(struct ext2_fs *fs, u32 inode_number,
                      const struct ext2_inode *inode, struct vfs_stat *out) {
    memset(out, 0, sizeof(*out));
    out->size = inode->size;
    out->inode = inode_number;
    out->blocks = inode->sectors_count;
    out->block_size = fs->block_size;
    out->mode = inode->mode;
    out->type = vfs_type(inode);
}

static int open_inode(struct ext2_fs *fs, u32 inode_number,
                      struct vfs_file *file) {
    struct ext2_inode *inode = ext2_inode_get(fs, inode_number);
    if (!inode || !inode->mode)
        return -1;
    struct ext2_open_file *opened = kmalloc(sizeof(*opened));
    if (!opened)
        return -1;
    opened->inode_number = inode_number;
    file->private_data = opened;
    file->position = 0;
    file->size = inode->size;
    file->inode = inode_number;
    file->type = vfs_type(inode);
    return 0;
}

static int ext2_backend_open(void *context, const char *path,
                             struct vfs_file *file) {
    struct ext2_fs *fs = context;
    u32 inode_number;
    if (ext2_path_resolve(fs, path, &inode_number) != 0)
        return -1;
    return open_inode(fs, inode_number, file);
}

static int ext2_backend_create(void *context, const char *path,
                               struct vfs_file *file) {
    struct ext2_fs *fs = context;
    u32 parent;
    char name[VFS_NAME_MAX + 1];
    if (ext2_path_parent(fs, path, &parent, name) != 0 ||
        ext2_dir_lookup(fs, parent, name, 0, 0) == 0)
        return -1;
    u32 inode_number;
    if (ext2_inode_allocate(fs, EXT2_S_IFREG | 0644u, &inode_number) != 0)
        return -1;
    struct ext2_inode *inode = ext2_inode_get(fs, inode_number);
    if (!inode || ext2_dir_add(fs, parent, name, inode_number,
                               EXT2_FT_REG_FILE) != 0) {
        if (inode)
            ext2_inode_free(fs, inode_number, inode);
        return -1;
    }
    return open_inode(fs, inode_number, file);
}

static void ext2_backend_close(void *context, struct vfs_file *file) {
    (void)context;
    if (file->private_data)
        kfree(file->private_data);
    file->private_data = 0;
}

static struct ext2_inode *file_inode(struct ext2_fs *fs,
                                     struct vfs_file *file) {
    struct ext2_open_file *opened = file ? file->private_data : 0;
    return opened ? ext2_inode_get(fs, opened->inode_number) : 0;
}

static void sync_file(struct vfs_file *file, const struct ext2_inode *inode) {
    file->size = inode->size;
    file->type = vfs_type(inode);
}

static usize ext2_backend_read(void *context, struct vfs_file *file,
                                void *buffer, usize length) {
    struct ext2_fs *fs = context;
    struct ext2_inode *inode = file_inode(fs, file);
    if (!inode || vfs_type(inode) != VFS_NODE_FILE)
        return 0;
    usize result = ext2_file_read(fs, inode, &file->position, buffer, length);
    sync_file(file, inode);
    return result;
}

static usize ext2_backend_write(void *context, struct vfs_file *file,
                                 const void *buffer, usize length) {
    struct ext2_fs *fs = context;
    struct ext2_inode *inode = file_inode(fs, file);
    if (!inode || vfs_type(inode) != VFS_NODE_FILE)
        return 0;
    usize result = ext2_file_write(fs, inode, &file->position, buffer, length);
    sync_file(file, inode);
    return result;
}

static int ext2_backend_seek(void *context, struct vfs_file *file,
                             u64 position) {
    (void)context;
    if (!file || position > UINT32_MAX)
        return -1;
    file->position = position;
    return 0;
}

static int ext2_backend_truncate(void *context, struct vfs_file *file) {
    struct ext2_fs *fs = context;
    struct ext2_inode *inode = file_inode(fs, file);
    if (!inode || vfs_type(inode) != VFS_NODE_FILE)
        return -1;
    ext2_inode_truncate(fs, inode);
    file->position = 0;
    sync_file(file, inode);
    return 0;
}

static int ext2_backend_stat(void *context, const char *path,
                             struct vfs_stat *out) {
    struct ext2_fs *fs = context;
    u32 inode_number;
    if (!out || ext2_path_resolve(fs, path, &inode_number) != 0)
        return -1;
    struct ext2_inode *inode = ext2_inode_get(fs, inode_number);
    if (!inode)
        return -1;
    fill_stat(fs, inode_number, inode, out);
    return 0;
}

static int ext2_backend_file_stat(void *context, struct vfs_file *file,
                                  struct vfs_stat *out) {
    struct ext2_fs *fs = context;
    struct ext2_open_file *opened = file ? file->private_data : 0;
    struct ext2_inode *inode = file_inode(fs, file);
    if (!opened || !inode || !out)
        return -1;
    fill_stat(fs, opened->inode_number, inode, out);
    return 0;
}

static int ext2_backend_unlink(void *context, const char *path) {
    struct ext2_fs *fs = context;
    u32 parent;
    char name[VFS_NAME_MAX + 1];
    u32 inode_number;
    u8 type;
    if (ext2_path_parent(fs, path, &parent, name) != 0 ||
        ext2_dir_lookup(fs, parent, name, &inode_number, &type) != 0 ||
        type == EXT2_FT_DIR)
        return -1;
    struct ext2_inode *inode = ext2_inode_get(fs, inode_number);
    if (!inode || ext2_dir_remove(fs, parent, name, 0, 0) != 0)
        return -1;
    if (inode->links_count > 1)
        inode->links_count--;
    else
        ext2_inode_free(fs, inode_number, inode);
    return 0;
}

static void set_directory_entry(struct ext2_dir_entry *entry,
                                u32 inode_number, u16 record_length,
                                const char *name, u8 name_length) {
    memset(entry, 0, record_length);
    entry->inode = inode_number;
    entry->record_length = record_length;
    entry->name_length = name_length;
    entry->file_type = EXT2_FT_DIR;
    memcpy(entry->name, name, name_length);
}

static int ext2_backend_mkdir(void *context, const char *path) {
    struct ext2_fs *fs = context;
    u32 parent_number;
    char name[VFS_NAME_MAX + 1];
    if (ext2_path_parent(fs, path, &parent_number, name) != 0 ||
        ext2_dir_lookup(fs, parent_number, name, 0, 0) == 0)
        return -1;
    u32 inode_number;
    if (ext2_inode_allocate(fs, EXT2_S_IFDIR | 0755u, &inode_number) != 0)
        return -1;
    struct ext2_inode *inode = ext2_inode_get(fs, inode_number);
    u32 block_number = inode ? ext2_inode_block(fs, inode, 0, 1) : 0;
    u8 *block = ext2_block(fs, block_number);
    if (!inode || !block) {
        if (inode)
            ext2_inode_free(fs, inode_number, inode);
        return -1;
    }
    inode->links_count = 2;
    inode->size = fs->block_size;
    set_directory_entry((struct ext2_dir_entry *)block, inode_number, 12,
                        ".", 1);
    set_directory_entry((struct ext2_dir_entry *)(block + 12), parent_number,
                        (u16)(fs->block_size - 12), "..", 2);
    if (ext2_dir_add(fs, parent_number, name, inode_number, EXT2_FT_DIR) != 0) {
        ext2_inode_free(fs, inode_number, inode);
        return -1;
    }
    struct ext2_inode *parent = ext2_inode_get(fs, parent_number);
    if (parent)
        parent->links_count++;
    return 0;
}

static int ext2_backend_rmdir(void *context, const char *path) {
    struct ext2_fs *fs = context;
    u32 parent_number;
    char name[VFS_NAME_MAX + 1];
    u32 inode_number;
    u8 type;
    if (ext2_path_parent(fs, path, &parent_number, name) != 0 ||
        ext2_dir_lookup(fs, parent_number, name, &inode_number, &type) != 0 ||
        type != EXT2_FT_DIR || inode_number == EXT2_ROOT_INODE ||
        !ext2_dir_empty(fs, inode_number))
        return -1;
    struct ext2_inode *inode = ext2_inode_get(fs, inode_number);
    if (!inode || ext2_dir_remove(fs, parent_number, name, 0, 0) != 0)
        return -1;
    struct ext2_inode *parent = ext2_inode_get(fs, parent_number);
    if (parent && parent->links_count)
        parent->links_count--;
    ext2_inode_free(fs, inode_number, inode);
    return 0;
}

static long ext2_backend_read_dir(void *context, const char *path,
                                  u32 *index, struct vfs_dirent *out) {
    struct ext2_fs *fs = context;
    u32 inode_number;
    if (ext2_path_resolve(fs, path, &inode_number) != 0)
        return -1;
    return ext2_dir_read_one(fs, inode_number, index, out);
}

const struct vfs_operations ext2_vfs_operations = {
    .open = ext2_backend_open,
    .create = ext2_backend_create,
    .close = ext2_backend_close,
    .read = ext2_backend_read,
    .write = ext2_backend_write,
    .seek = ext2_backend_seek,
    .truncate = ext2_backend_truncate,
    .stat = ext2_backend_stat,
    .file_stat = ext2_backend_file_stat,
    .unlink = ext2_backend_unlink,
    .mkdir = ext2_backend_mkdir,
    .rmdir = ext2_backend_rmdir,
    .read_dir = ext2_backend_read_dir,
};

extern const struct vfs_filesystem ext2_filesystem;

void ext2_vfs_register(void) {
    vfs_register(&ext2_filesystem);
}
