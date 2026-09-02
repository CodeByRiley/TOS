/* Exercise TOS's ext2 backend against an image made by mke2fs. */
#include "fs/ext2/ext2.h"
#include "fs/vfs/vfs.h"
#include "vfs_backend_checks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void expect(int condition, const char *message) {
    if (condition)
        return;
    printf("FAIL: %s\n", message);
    failed = 1;
}

static unsigned char *load_image(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return 0;
    }
    unsigned char *image = malloc((size_t)length);
    if (!image || fread(image, 1, (size_t)length, file) != (size_t)length) {
        free(image);
        fclose(file);
        return 0;
    }
    fclose(file);
    *size_out = (size_t)length;
    return image;
}

static int save_image(const char *path, const void *image, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    int result = fwrite(image, 1, size, file) == size ? 0 : -1;
    fclose(file);
    return result;
}

static int backend_check(int condition, const char *message) {
    expect(condition, message);
    return !condition;
}

static int packed_names_contain(const char *names, long bytes,
                                const char *wanted) {
    for (long offset = 0; offset < bytes;) {
        const char *name = names + offset;
        size_t length = strlen(name);
        if (strcmp(name, wanted) == 0)
            return 1;
        offset += (long)length + 1;
    }
    return 0;
}

int main(void) {
    const char *source = "build/tests/ext2-base.img";
    const char *mutated = "build/tests/ext2-mutated.img";
    size_t image_size = 0;
    unsigned char *image = load_image(source, &image_size);
    expect(image != 0, "load mke2fs image");
    if (!image)
        return 1;

    vfs_init();
    ext2_vfs_register();
    const char *mounted_type = 0;
    expect(vfs_mount_auto("/", image, image_size, &mounted_type) == 0 &&
           mounted_type && strcmp(mounted_type, "ext2") == 0,
           "auto-detect and mount ext2");

    unsigned char *mounted_image = malloc(image_size);
    expect(mounted_image != 0, "allocate nested mount image");
    if (mounted_image) {
        memcpy(mounted_image, image, image_size);
        expect(vfs_mount_image("/mnt", "ext2", mounted_image, image_size) == 0,
               "mount a second filesystem at /mnt");
        struct vfs_file mounted_file;
        char mounted_text[32] = {0};
        expect(vfs_open("/mnt/seed/hello.txt", &mounted_file) == 0 &&
               vfs_read(&mounted_file, mounted_text, sizeof(mounted_text)) ==
                   27 &&
               memcmp(mounted_text, "hello from an ext2 fixture\n", 27) == 0,
               "longest-prefix mount routing uses the nested filesystem");
        vfs_close(&mounted_file);
        expect(vfs_open("/mntish/seed/hello.txt", &mounted_file) != 0,
               "mount routing requires a complete path component");
    }

    struct vfs_stat metadata;
    expect(vfs_stat("/seed/hello.txt", &metadata) == 0 &&
           metadata.type == VFS_NODE_FILE && metadata.size == 27,
           "stat fixture file");
    expect(vfs_stat("/SEED/hello.txt", &metadata) != 0,
           "ext2 paths remain case-sensitive");

    struct vfs_file file;
    char text[64] = {0};
    expect(vfs_open("/seed/hello.txt", &file) == 0,
           "open fixture file");
    expect(vfs_read(&file, text, sizeof(text)) == 27 &&
           memcmp(text, "hello from an ext2 fixture\n", 27) == 0,
           "read fixture contents");
    vfs_close(&file);

    char names[512];
    uint32_t index = 0;
    long name_bytes = vfs_read_dir("/", &index, names, sizeof(names));
    expect(name_bytes > 0 && packed_names_contain(names, name_bytes, "seed/") &&
           packed_names_contain(names, name_bytes, "lost+found/"),
           "enumerate ext2 root directory");

    expect(vfs_mkdir("/work") == 0 && vfs_mkdir("/work/sub") == 0,
           "create nested directories");
    expect(vfs_create("/work/sub/large.bin", &file) == 0,
           "create nested file");
    enum { PAYLOAD_SIZE = 300 * 1024 + 333 };
    unsigned char *payload = malloc(PAYLOAD_SIZE);
    unsigned char *readback = malloc(PAYLOAD_SIZE);
    expect(payload && readback, "allocate large-file buffers");
    if (payload && readback) {
        for (size_t i = 0; i < PAYLOAD_SIZE; i++)
            payload[i] = (unsigned char)((i * 37u + 11u) & 0xffu);
        expect(vfs_write(&file, payload, PAYLOAD_SIZE) == PAYLOAD_SIZE,
               "write through single and double-indirect ext2 blocks");
        expect(vfs_seek(&file, 0) == 0 &&
               vfs_read(&file, readback, PAYLOAD_SIZE) == PAYLOAD_SIZE &&
               memcmp(payload, readback, PAYLOAD_SIZE) == 0,
               "large ext2 file round-trips");
    }
    vfs_close(&file);

    expect(vfs_create("/work/sparse.bin", &file) == 0,
           "create sparse file");
    expect(vfs_seek(&file, 4096) == 0 &&
           vfs_write(&file, "end", 3) == 3 &&
           vfs_seek(&file, 0) == 0,
           "seek beyond EOF and write sparse tail");
    unsigned char sparse[4099];
    memset(sparse, 0xcc, sizeof(sparse));
    expect(vfs_read(&file, sparse, sizeof(sparse)) == sizeof(sparse) &&
           sparse[0] == 0 && sparse[4095] == 0 &&
           memcmp(sparse + 4096, "end", 3) == 0,
           "sparse hole reads as zeroes");
    expect(vfs_truncate(&file) == 0 && file.size == 0,
           "truncate frees ext2 data blocks");
    vfs_close(&file);

    expect(vfs_rmdir("/work/sub") != 0,
           "refuse to remove a non-empty directory");
    expect(vfs_unlink("/work/sub/large.bin") == 0 &&
           vfs_rmdir("/work/sub") == 0,
           "unlink file then remove empty directory");
    expect(vfs_unlink("/work/sparse.bin") == 0 && vfs_rmdir("/work") == 0,
           "release remaining directory contents");
    expect(vfs_stat("/work", &metadata) != 0,
           "removed directory is absent");

    vfs_backend_checks(backend_check);
    expect(save_image(mutated, image, image_size) == 0,
           "save mutated ext2 image for e2fsck");
    free(payload);
    free(readback);
    if (mounted_image)
        expect(vfs_unmount("/mnt") == 0, "unmount nested ext2");
    expect(vfs_unmount("/") == 0, "unmount ext2 before releasing image");
    free(mounted_image);
    free(image);
    if (!failed)
        printf("ext2_vfs_test: all checks passed\n");
    return failed;
}
