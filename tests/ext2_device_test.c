/* Real ext2 mutations against a sector device, including persistence errors. */
#include "fs/ext2/ext2.h"
#include "drivers/storage/block.h"
#include "vfs_backend_checks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct test_disk {
    unsigned char *bytes;
    size_t size;
    unsigned writes, flushes;
    unsigned fail_write;
    int fail_read, fail_flush;
};
static int failures;
static int check(int condition, const char *message) {
    if (!condition) { printf("FAIL: %s\n", message); failures++; }
    return !condition;
}
static int read_sectors(void *context, uint64_t lba, uint32_t count, void *out) {
    struct test_disk *disk = context;
    if (disk->fail_read || lba * 512 + (uint64_t)count * 512 > disk->size) return -1;
    memcpy(out, disk->bytes + lba * 512, (size_t)count * 512);
    return 0;
}
static int write_sectors(void *context, uint64_t lba, uint32_t count, const void *in) {
    struct test_disk *disk = context;
    disk->writes++;
    if (disk->writes == disk->fail_write || lba * 512 + (uint64_t)count * 512 > disk->size)
        return -1;
    memcpy(disk->bytes + lba * 512, in, (size_t)count * 512);
    return 0;
}
static int flush(void *context) {
    struct test_disk *disk = context;
    disk->flushes++;
    return disk->fail_flush ? -1 : 0;
}

int main(int argc, char **argv) {
    FILE *input = fopen(argc > 1 ? argv[1] : "build/tests/ext2-base.img", "rb");
    if (!input) return 1;
    fseek(input, 0, SEEK_END);
    long size = ftell(input);
    rewind(input);
    struct test_disk disk = { .size = (size_t)size };
    disk.bytes = malloc(disk.size);
    if (!disk.bytes || fread(disk.bytes, 1, disk.size, input) != disk.size) return 1;
    fclose(input);
    struct block_device device = { .context = &disk, .sectors = disk.size / 512,
        .read = read_sectors, .write = write_sectors, .flush = flush };
    vfs_init(); ext2_vfs_register();
    disk.fail_read = 1;
    check(ext2_mount_device("/", &device) < 0, "failed device read does not publish mount");
    disk.fail_read = 0;
    check(ext2_mount_device("/", &device) == 0, "mount sector-backed ext2");
    struct vfs_stat metadata;
    check(vfs_stat("/seed/hello.txt", &metadata) == 0 && disk.writes == 0,
          "read-only workload does not dirty disk");
    vfs_backend_checks(check);
    size_t LENGTH = (12 + metadata.block_size / 4 + 2) * metadata.block_size + 333;
    unsigned char *payload = malloc(LENGTH), *readback = malloc(LENGTH);
    if (!payload || !readback) return 1;
    for (size_t i = 0; i < LENGTH; i++) payload[i] = (unsigned char)(i * 37u + 11u);
    struct vfs_file file = {0};
    check(vfs_create("/persist", &file) == 0 && vfs_write(&file, payload, LENGTH) == LENGTH,
          "write direct, single- and double-indirect blocks to device");
    check(vfs_file_sync(&file) == 0 && disk.flushes >= 3, "sync reaches device barriers");
    vfs_close(&file);
    check(vfs_unmount("/") == 0 && ext2_mount_device("/", &device) == 0,
          "remount allocates a fresh image from persisted sectors");
    check(vfs_open("/persist", &file) == 0 && vfs_read(&file, readback, LENGTH) == LENGTH &&
          !memcmp(payload, readback, LENGTH), "fresh mount reproduces every persisted byte");
    check(vfs_truncate(&file) == 0, "truncate persists allocation metadata");
    /* Sparse seek reaches the first triple-indirect branch without allocating
     * gigabytes of data. Skip it when beyond the supported 32-bit file size. */
    uint64_t pointers = metadata.block_size / 4;
    uint64_t triple = (12 + pointers + pointers * pointers) * metadata.block_size;
    if (triple + 5 <= UINT32_MAX) {
        check(vfs_seek(&file, triple) == 0 && vfs_write(&file, "tree", 5) == 5,
              "write sparse triple-indirect data");
        vfs_close(&file);
        check(vfs_unmount("/") == 0 && ext2_mount_device("/", &device) == 0 &&
              vfs_open("/persist", &file) == 0 && vfs_seek(&file, triple) == 0 &&
              vfs_read(&file, readback, 5) == 5 && !memcmp(readback, "tree", 5),
              "triple-indirect data survives remount");
        check(vfs_truncate(&file) == 0 && vfs_seek(&file, 0) == 0,
              "truncate releases every indirect level");
    }
    disk.fail_write = disk.writes + 2; /* Fail after the unclean marker is durable. */
    check(vfs_write(&file, "retry", 5) == 0, "writeback failure is reported");
    unsigned before = disk.writes;
    check(vfs_write(&file, "bad", 3) == 0 && disk.writes == before,
          "failed filesystem rejects further mutation");
    check(!(disk.bytes[1024 + 58] & 1), "interrupted write leaves disk unclean");
    disk.fail_write = 0;
    check(vfs_file_sync(&file) == 0, "explicit sync retries retained dirty blocks");
    check(vfs_seek(&file, 0) == 0 && vfs_write(&file, "good", 4) == 4,
          "successful retry restores writable state");
    disk.fail_flush = 1;
    check(vfs_truncate(&file) < 0, "cache-flush failure is reported");
    disk.fail_flush = 0;
    check(vfs_file_sync(&file) == 0, "flush failure can be retried");
    vfs_close(&file);
    check(vfs_unlink("/persist") == 0 && vfs_unmount("/") == 0,
          "delete and unmount persist clean metadata");
    check(ext2_mount_device("/", &device) == 0 && vfs_stat("/persist", &metadata) < 0,
          "deleted file stays absent after remount");
    check(vfs_unmount("/") == 0, "release final device mount");
    FILE *output = fopen(argc > 2 ? argv[2] : "build/tests/ext2-device.img", "wb");
    check(output && fwrite(disk.bytes, 1, disk.size, output) == disk.size,
          "save persisted image for e2fsck");
    if (output) fclose(output);
    free(payload); free(readback); free(disk.bytes);
    if (!failures) puts("ext2_device_test: all checks passed");
    return failures ? 1 : 0;
}
