#include <fs/fat/fat.h>
#include <fs/fat/fat_vfs.h>
#include <fs/vfs/vfs.h>
#include <memory/heap.h>
#include <utilities/string.h>

#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IRUSR 0000400u
#define S_IWUSR 0000200u
#define S_IXUSR 0000100u
#define S_IRGRP 0000040u
#define S_IWGRP 0000020u
#define S_IXGRP 0000010u
#define S_IROTH 0000004u
#define S_IWOTH 0000002u
#define S_IXOTH 0000001u

static char fat_context;

static void sync_file(struct vfs_file *file, const struct fat_file *fat) {
    file->position = fat->pos;
    file->size = fat->size;
    file->inode = fat->first_cluster ? fat->first_cluster : 1;
    file->type = VFS_NODE_FILE;
}

static int fat_backend_open(void *fs, const char *path,
                            struct vfs_file *file) {
    (void)fs;
    struct fat_stat metadata;
    if (fat_stat(path, &metadata) != 0)
        return -1;
    if (metadata.is_dir) {
        file->size = 0;
        file->inode = metadata.first_cluster ? metadata.first_cluster : 1;
        file->type = VFS_NODE_DIRECTORY;
        file->attributes = metadata.attr;
        return 0;
    }
    struct fat_file *fat = kmalloc(sizeof(*fat));
    if (!fat)
        return -1;
    if (fat_open(path, fat) != 0) {
        kfree(fat);
        return -1;
    }
    file->private_data = fat;
    sync_file(file, fat);
    return 0;
}

static int fat_backend_create(void *fs, const char *path,
                              struct vfs_file *file) {
    (void)fs;
    struct fat_file *fat = kmalloc(sizeof(*fat));
    if (!fat)
        return -1;
    if (fat_create(path, fat) != 0) {
        kfree(fat);
        return -1;
    }
    file->private_data = fat;
    sync_file(file, fat);
    return 0;
}

static void fat_backend_close(void *fs, struct vfs_file *file) {
    (void)fs;
    if (file->private_data)
        kfree(file->private_data);
    file->private_data = 0;
}

static usize fat_backend_read(void *fs, struct vfs_file *file, void *buffer,
                               usize length) {
    (void)fs;
    struct fat_file *fat = file->private_data;
    usize result = fat_read(fat, buffer, length);
    sync_file(file, fat);
    return result;
}

static usize fat_backend_write(void *fs, struct vfs_file *file,
                                const void *buffer, usize length) {
    (void)fs;
    struct fat_file *fat = file->private_data;
    usize result = fat_write(fat, buffer, length);
    sync_file(file, fat);
    return result;
}

static int fat_backend_seek(void *fs, struct vfs_file *file,
                            u64 position) {
    (void)fs;
    if (position > UINT32_MAX)
        return -1;
    struct fat_file *fat = file->private_data;
    int result = fat_seek(fat, (u32)position);
    sync_file(file, fat);
    return result;
}

static int fat_backend_truncate(void *fs, struct vfs_file *file) {
    (void)fs;
    struct fat_file *fat = file->private_data;
    int result = fat_truncate(fat);
    sync_file(file, fat);
    return result;
}

static void fat_stat_to_vfs(const struct fat_stat *fat, struct vfs_stat *out) {
    memset(out, 0, sizeof(*out));
    out->size = fat->size;
    out->inode = fat->first_cluster ? fat->first_cluster : 1;
    out->blocks = (fat->size + 511u) / 512u;
    out->block_size = 4096;
    out->type = fat->is_dir ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    out->attributes = fat->attr;
    u32 permissions = (fat->attr & 0x01)
        ? (S_IRUSR | S_IRGRP | S_IROTH)
        : (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (fat->is_dir)
        out->mode = S_IFDIR | permissions | S_IXUSR | S_IXGRP | S_IXOTH;
    else
        out->mode = S_IFREG | permissions;
}

static int fat_backend_stat(void *fs, const char *path, struct vfs_stat *out) {
    (void)fs;
    struct fat_stat fat;
    if (fat_stat(path, &fat) != 0)
        return -1;
    fat_stat_to_vfs(&fat, out);
    return 0;
}

static int fat_backend_file_stat(void *fs, struct vfs_file *file,
                                 struct vfs_stat *out) {
    (void)fs;
    struct fat_file *fat = file->private_data;
    memset(out, 0, sizeof(*out));
    out->size = fat->size;
    out->inode = fat->first_cluster ? fat->first_cluster : 1;
    out->blocks = (fat->size + 511u) / 512u;
    out->block_size = 4096;
    out->type = VFS_NODE_FILE;
    out->mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                S_IROTH | S_IWOTH;
    return 0;
}

static int fat_backend_unlink(void *fs, const char *path) {
    (void)fs;
    return fat_unlink(path);
}
static int fat_backend_mkdir(void *fs, const char *path) {
    (void)fs;
    return fat_mkdir(path);
}
static int fat_backend_rmdir(void *fs, const char *path) {
    (void)fs;
    return fat_rmdir(path);
}

static long fat_backend_read_dir(void *fs, const char *path, u32 *index,
                                 struct vfs_dirent *out) {
    (void)fs;
    int is_directory = 0;
    long result = fat_read_dir_one(path, index, out->name, sizeof(out->name),
                                   &is_directory);
    if (result <= 0)
        return result;
    out->inode = *index ? *index : 1;
    out->type = is_directory ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    return result;
}

static const struct vfs_operations fat_operations = {
    .open = fat_backend_open,
    .create = fat_backend_create,
    .close = fat_backend_close,
    .read = fat_backend_read,
    .write = fat_backend_write,
    .seek = fat_backend_seek,
    .truncate = fat_backend_truncate,
    .stat = fat_backend_stat,
    .file_stat = fat_backend_file_stat,
    .unlink = fat_backend_unlink,
    .mkdir = fat_backend_mkdir,
    .rmdir = fat_backend_rmdir,
    .read_dir = fat_backend_read_dir,
};

static int fat_probe(const void *image, usize size) {
    if (!image || size < 512)
        return 0;
    usize volume_size = fat_volume_size(image, 0);
    return volume_size && volume_size <= size;
}

static int fat_mount_image(void *image, usize size, void **fs_out) {
    if (fat_init(image, size) != 0)
        return -1;
    *fs_out = &fat_context;
    return 0;
}

static const struct vfs_filesystem fat_filesystem = {
    .name = "fat",
    .probe = fat_probe,
    .mount = fat_mount_image,
    .unmount = 0,
    .operations = &fat_operations,
};

void fat_vfs_register(void) {
    vfs_register(&fat_filesystem);
}

int fat_vfs_attach(const char *mountpoint) {
    return vfs_attach(mountpoint, "fat", &fat_context);
}
