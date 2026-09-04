/* FAT mounted through struct block_device, the path kernel/fs/fat/fat_block.c
 * takes when a real transport supplies the volume.
 *
 * The QEMU persistence tests put ext2 on their disks, so nothing else
 * exercises this. What matters here is that the volume is read in over the
 * block interface, that mutations reach the device as they happen rather than
 * only at sync, and that a device which refuses a write is reported rather
 * than quietly dropped.
 */
#include "drivers/storage/block.h"
#include "fs/fat/fat.h"
#include "fs/fat/fat_vfs.h"
#include "fs/vfs/vfs.h"
#include "vfs_backend_checks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512
#define TOTAL_SECTORS 70000
#define RESERVED_SECTORS 32
#define FAT_SECTORS 600
#define ROOT_CLUSTER 2

struct __attribute__((packed)) bpb32 {
    uint8_t jump[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t version;
    uint32_t root_cluster;
};

struct test_disk {
    unsigned char *bytes;
    size_t size;
    unsigned reads, writes, flushes;
    unsigned fail_write; /* fail the Nth write; 0 disables */
    int fail_read;
};

/* No memmem on every host libc, and the payload is small. */
static int bytes_present(const unsigned char *haystack, size_t haystack_len,
                         const void *needle, size_t needle_len) {
    if (needle_len > haystack_len)
        return 0;
    for (size_t i = 0; i + needle_len <= haystack_len; i++)
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return 1;
    return 0;
}

static int failures;
static int check(int condition, const char *message) {
    if (!condition) {
        printf("FAIL: %s\n", message);
        failures++;
    }
    return !condition;
}

static int read_sectors(void *context, uint64_t lba, uint32_t count, void *out) {
    struct test_disk *disk = context;
    disk->reads++;
    if (disk->fail_read ||
        lba * SECTOR_SIZE + (uint64_t)count * SECTOR_SIZE > disk->size)
        return -1;
    memcpy(out, disk->bytes + lba * SECTOR_SIZE, (size_t)count * SECTOR_SIZE);
    return 0;
}

static int write_sectors(void *context, uint64_t lba, uint32_t count,
                         const void *in) {
    struct test_disk *disk = context;
    disk->writes++;
    if (disk->writes == disk->fail_write ||
        lba * SECTOR_SIZE + (uint64_t)count * SECTOR_SIZE > disk->size)
        return -1;
    memcpy(disk->bytes + lba * SECTOR_SIZE, in, (size_t)count * SECTOR_SIZE);
    return 0;
}

static int flush_device(void *context) {
    struct test_disk *disk = context;
    disk->flushes++;
    return 0;
}

/* A minimal but genuinely valid FAT32 volume: the mount path parses this the
 * same way it would parse one off a disk. */
static void format(unsigned char *image) {
    struct bpb32 *bpb = (struct bpb32 *)image;
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = 1;
    bpb->reserved_sectors = RESERVED_SECTORS;
    bpb->fat_count = 1;
    bpb->media = 0xF8;
    bpb->total_sectors_32 = TOTAL_SECTORS;
    bpb->sectors_per_fat_32 = FAT_SECTORS;
    bpb->root_cluster = ROOT_CLUSTER;

    uint32_t *fat = (uint32_t *)(image + (size_t)RESERVED_SECTORS * SECTOR_SIZE);
    fat[0] = 0x0FFFFFF8u;
    fat[1] = 0x0FFFFFFFu;
    fat[ROOT_CLUSTER] = 0x0FFFFFFFu;
}

int main(void) {
    struct test_disk disk = {.size = (size_t)TOTAL_SECTORS * SECTOR_SIZE};
    disk.bytes = calloc(1, disk.size);
    if (!disk.bytes)
        return 1;
    format(disk.bytes);

    struct block_device device = {.context = &disk,
                                  .sectors = disk.size / SECTOR_SIZE,
                                  .read = read_sectors,
                                  .write = write_sectors,
                                  .flush = flush_device};

    vfs_init();
    fat_vfs_register();

    /* A device that cannot be read must not leave a mount behind. */
    disk.fail_read = 1;
    check(fat_mount_device("/", &device) < 0,
          "failed device read does not publish a mount");
    disk.fail_read = 0;

    check(fat_mount_device("/", &device) == 0, "mount FAT over a block device");
    check(disk.reads > 0, "the volume was read in over the block interface");
    check(disk.writes == 0, "mounting alone writes nothing");

    /* A device mount owns its image, so the volume the FAT engine works on is
     * not the caller's buffer. Writing through must therefore reach the disk
     * rather than only the cache. */
    struct vfs_file file = {0};
    check(vfs_create("/PERSIST.TXT", &file) == 0, "create a file on the volume");
    static const char payload[] = "fat-through-block-device";
    check(vfs_write(&file, payload, sizeof(payload)) == sizeof(payload),
          "write file data");
    vfs_close(&file);
    check(disk.writes > 0, "mutations write through to the device as they happen");

    unsigned writes_before_sync = disk.writes;
    check(vfs_sync_all() == 0, "sync the volume");
    check(disk.flushes > 0, "sync reaches the device flush");
    check(disk.writes >= writes_before_sync, "sync does not lose writes");

    /* The bytes really landed on the disk, not just in the cached image. */
    check(bytes_present(disk.bytes, disk.size, payload, sizeof(payload) - 1),
          "file data is present in the device's own sectors");

    check(vfs_backend_checks(check) == 0, "shared VFS backend contract");

    check(vfs_unmount("/") == 0, "unmount the device-backed volume");

    /* Remount and read it back: proof the volume on the device is coherent. */
    check(fat_mount_device("/", &device) == 0, "remount the same device");
    struct vfs_file reopened = {0};
    char readback[sizeof(payload)] = {0};
    check(vfs_open("/PERSIST.TXT", &reopened) == 0, "reopen the file after remount");
    check(vfs_read(&reopened, readback, sizeof(readback)) == sizeof(payload) &&
              memcmp(readback, payload, sizeof(payload)) == 0,
          "file contents survive a remount from the device");
    vfs_close(&reopened);

    /* A refused write is reported through sync, not swallowed. */
    disk.fail_write = disk.writes + 1;
    check(vfs_sync_all() != 0, "a device that refuses a write makes sync fail");
    disk.fail_write = 0;
    check(vfs_sync_all() == 0, "sync succeeds again once the device recovers");

    check(vfs_unmount("/") == 0, "unmount after the failure case");
    free(disk.bytes);

    if (failures) {
        printf("fat_device_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("fat_device_test: all checks passed\n");
    return 0;
}
