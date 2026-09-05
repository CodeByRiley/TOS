/* VFS contracts tested without any disk format. Allocation failures are
 * injected here so leaked path/inode/open-file references cannot hide. */
#include "fs/vfs/vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed, allocations, fail_after = -1, fail_open;
static int mounts, unmounts, reads, releases;
static const struct vfs_inode_operations inode_ops;
static const struct vfs_file_operations file_ops;

void *kmalloc(size_t size) {
    if (fail_after == 0) return 0;
    if (fail_after > 0) fail_after--;
    void *memory = malloc(size);
    if (memory) allocations++;
    return memory;
}
void kfree(void *memory) {
    if (memory) { allocations--; free(memory); }
}
static void expect(int condition, const char *message) {
    if (!condition) { printf("FAIL: %s\n", message); failed = 1; }
}
static int packed_has(const char *entries, long bytes, const char *name) {
    for (long at = 0; at < bytes; at += (long)strlen(entries + at) + 1)
        if (!strcmp(entries + at, name)) return 1;
    return 0;
}

static struct vfs_inode *get_inode(struct vfs_superblock *super, uint64_t number) {
    return vfs_inode_get(super, number, number == 3 ? VFS_NODE_FILE : VFS_NODE_DIRECTORY,
                          0, &inode_ops, &file_ops);
}
static int lookup(struct vfs_inode *dir, const char *name, struct vfs_inode **out) {
    uint64_t number = 0;
    if (dir->number == 1 && !strcmp(name, "dir")) number = 2;
    if ((dir->number == 1 || dir->number == 2) && !strcmp(name, "file")) number = 3;
    if (!number) return -1;
    *out = get_inode(dir->super, number);
    return *out ? 0 : -1;
}
static int getattr(struct vfs_inode *inode, struct vfs_stat *out) {
    *out = (struct vfs_stat){ .inode = inode->number, .type = inode->type,
        .size = inode->type == VFS_NODE_FILE ? 4 : 0 };
    return 0;
}
static int open_file(struct vfs_file *file) {
    file->private_data = kmalloc(1);
    return !file->private_data || fail_open ? -1 : 0;
}
static void release_file(struct vfs_file *file) {
    releases++;
    kfree(file->private_data);
}
static size_t read_file(struct vfs_file *file, void *buffer, size_t length) {
    reads++;
    if (file->position >= 4) return 0;
    if (length > 4 - file->position) length = 4 - file->position;
    memcpy(buffer, (char *)file->node->super->private_data + file->position, length);
    file->position += length;
    return length;
}
static int seek_file(struct vfs_file *file, uint64_t position) {
    file->position = position;
    return 0;
}
static long iterate(struct vfs_inode *dir, uint32_t *index, struct vfs_dirent *out) {
    (void)dir;
    if (*index >= 2) return 0;
    out->inode = *index ? 3 : 2;
    out->type = *index ? VFS_NODE_FILE : VFS_NODE_DIRECTORY;
    strcpy(out->name, *index ? "file" : "dir");
    (*index)++;
    return 1;
}
static int mount_image(struct vfs_superblock *super, void *image, size_t size) {
    (void)size;
    mounts++;
    super->private_data = image;
    super->root = get_inode(super, 1);
    return super->root ? 0 : -1;
}
static void unmount_image(struct vfs_superblock *super) {
    (void)super;
    unmounts++;
}
static const struct vfs_inode_operations inode_ops = { .lookup = lookup, .getattr = getattr };
static const struct vfs_file_operations file_ops = { .open = open_file,
    .release = release_file, .read = read_file, .seek = seek_file, .iterate = iterate };
static const struct vfs_filesystem filesystem = {
    .name = "test", .mount = mount_image, .unmount = unmount_image,
};

int main(void) {
    vfs_init();
    expect(vfs_register(0) < 0 && vfs_register(&filesystem) == 0 &&
           vfs_register(&filesystem) < 0, "registration validates duplicates and null types");
    struct vfs_file file, other;
    struct vfs_stat st;
    char data[16] = {0};
    expect(vfs_open("/file", &file) < 0, "lookup without root mount fails");
    expect(vfs_mount_image("/", "test", "root", 4) == 0, "mount root");
    expect(vfs_mount_image("/", "test", "oops", 4) < 0 && mounts == 1,
           "duplicate rejected before backend mutation");
    expect(vfs_mount_image("/mnt/", "test", "nest", 4) == 0, "mount nested root");
    expect(vfs_open("/mnt//dir/../file", &file) == 0 &&
           vfs_read(&file, data, 4) == 4 && !memcmp(data, "nest", 4),
           "component lookup crosses mount and skips repeated separators");
    vfs_close(&file);
    expect(vfs_open("/mnt/../file", &file) == 0 &&
           vfs_read(&file, data, 4) == 4 && !memcmp(data, "root", 4),
           "dot-dot exits a mounted filesystem");
    vfs_close(&file);
    expect(vfs_stat("/../../file", &st) == 0, "dot-dot cannot escape root");
    expect(vfs_stat("/mntish/file", &st) < 0, "mount names match complete components");
    expect(vfs_stat("/missing/../file", &st) < 0, "dot-dot cannot skip a missing component");
    expect(vfs_stat("/file/..", &st) < 0 && vfs_stat("/file/.", &st) < 0 &&
           vfs_stat("/file/", &st) < 0, "non-directories cannot be traversed");
    expect(vfs_open("/file", &file) == 0 && vfs_open("/dir/file", &other) == 0 &&
           file.node == other.node, "inode identity is shared across names");
    expect(vfs_read(&file, data, 2) == 2 && other.position == 0,
           "independent opens retain independent cursors");
    uint32_t mounts_index = 0;
    char mount_names[32];
    long mount_bytes = vfs_read_dir("/", &mounts_index, mount_names,
                                    sizeof(mount_names));
    expect(mount_bytes > 0 && packed_has(mount_names, mount_bytes, "mnt/"),
           "directory listings include direct child mountpoints");
    expect(vfs_unmount("/") < 0 && vfs_unmount("/mnt") == 0,
           "open root is busy but unused child can unmount");
    vfs_close(&file);
    vfs_close(&other);
    int before_reads = reads;
    expect(vfs_open("/dir", &file) == 0 && vfs_file_stat(&file, &st) == 0 &&
           st.type == VFS_NODE_DIRECTORY && vfs_read(&file, data, 1) == 0 &&
           vfs_write(&file, data, 1) == 0 && vfs_seek(&file, 0) < 0 &&
           vfs_truncate(&file) < 0 && reads == before_reads,
           "directory metadata works; byte operations never reach backend");
    vfs_close(&file);
    int before_releases = releases;
    vfs_close(&file);
    expect(releases == before_releases, "close is idempotent");

    uint32_t index = 0;
    expect(vfs_read_dir("/", &index, data, 4) < 0 && index == 0,
           "short directory buffer preserves cookie");
    expect(vfs_read_dir("/", &index, data, 5) == 5 && index == 1 && !strcmp(data, "dir/"),
           "packed directories retain trailing slash");
    expect(vfs_read_dir("/", &index, data, sizeof(data)) == 5 && index == 2 &&
           !strcmp(data, "file"), "enumeration resumes at unconsumed entry");
    expect(vfs_stat(0, &st) < 0 && vfs_stat("/", 0) < 0 &&
           vfs_open(0, &file) < 0 && vfs_open("relative", &file) < 0 &&
           vfs_read(0, data, 1) == 0 && vfs_read_dir_one("/", 0, 0) < 0,
           "null and relative inputs fail safely");
    char long_path[VFS_PATH_MAX + 1];
    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/'; long_path[sizeof(long_path) - 1] = 0;
    expect(vfs_stat(long_path, &st) < 0, "overlong paths rejected");

    int baseline = allocations;
    for (int limit = 0; limit < 10; limit++) {
        fail_after = limit;
        int result = vfs_open("/dir/file", &file);
        fail_after = -1;
        if (!result) vfs_close(&file);
        expect(allocations == baseline, "all lookup/open allocation failures release references");
    }
    fail_open = 1;
    expect(vfs_open("/file", &file) < 0 && !file.node && allocations == baseline,
           "failed backend open releases partial private state");
    fail_open = 0;
    expect(vfs_create("/new", &file) < 0 && vfs_mkdir("/new") < 0 &&
           vfs_unlink("/file") < 0, "missing operation hooks fail safely");
    for (int i = 1; i < VFS_MAX_MOUNTS; i++) {
        char path[16]; snprintf(path, sizeof(path), "/mount%d", i);
        expect(vfs_mount_image(path, "test", "more", 4) == 0, "fill mount table");
    }
    int before_mounts = mounts;
    expect(vfs_mount_image("/overflow", "test", "more", 4) < 0 && mounts == before_mounts,
           "full table rejected before calling backend");
    expect(vfs_unmount("/") < 0, "parent mount is busy while child mounts exist");
    for (int i = 1; i < VFS_MAX_MOUNTS; i++) {
        char path[16]; snprintf(path, sizeof(path), "/mount%d", i);
        expect(vfs_unmount(path) == 0, "unmount child");
    }
    expect(vfs_unmount("/") == 0 && allocations == 0 && mounts == unmounts,
           "unmount releases all superblock and inode ownership");
    fail_after = 0;
    expect(vfs_mount_image("/", "test", "root", 4) < 0, "failed root allocation rolls mount back");
    fail_after = -1;
    expect(allocations == 0 && mounts == unmounts &&
           vfs_mount_image("/", "test", "root", 4) == 0 && vfs_unmount("/") == 0,
           "failed mount leaves reusable slot");
    if (!failed) puts("vfs_test: all checks passed");
    return failed;
}
