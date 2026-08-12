/* Host-side regression test for the kernel FAT16 directory implementation. */
#include "fs/fat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512
#define TOTAL_SECTORS 6000
#define FAT_SECTORS 24
#define ROOT_ENTRIES 32

struct __attribute__((packed)) test_bpb {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t  media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t large_total_sectors;
};

/* fat.c only logs geometry during initialization. */
void log_write_hex(const char *message, uint64_t value,
                   uint8_t type, uint8_t level) {
    (void)message;
    (void)value;
    (void)type;
    (void)level;
}

static int contains_name(const char *buffer, long length,
                         const char *wanted) {
    long offset = 0;
    while (offset < length) {
        const char *name = buffer + offset;
        if (strcmp(name, wanted) == 0)
            return 1;
        offset += (long)strlen(name) + 1;
    }
    return 0;
}

static int expect(int condition, const char *message) {
    if (condition)
        return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    size_t image_size = (size_t)TOTAL_SECTORS * SECTOR_SIZE;
    uint8_t *image = calloc(1, image_size);
    if (!image)
        return 1;

    struct test_bpb *bpb = (struct test_bpb *)image;
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = 1;
    bpb->reserved_sectors = 1;
    bpb->fat_count = 1;
    bpb->root_entries = ROOT_ENTRIES;
    bpb->total_sectors = TOTAL_SECTORS;
    bpb->media = 0xF8;
    bpb->sectors_per_fat = FAT_SECTORS;

    uint16_t *fat = (uint16_t *)(image + SECTOR_SIZE);
    fat[0] = 0xFFF8;
    fat[1] = 0xFFFF;

    int failed = 0;
    failed |= expect(fat_init(image, image_size) == 0,
                     "initialize synthetic FAT16 image");
    failed |= expect(fat_mkdir("/APPS") == 0, "create /APPS");
    failed |= expect(fat_mkdir("APPS/TOOLS") == 0,
                     "create nested APPS/TOOLS");
    failed |= expect(fat_mkdir("apps/tools") != 0,
                     "reject duplicate directory case-insensitively");
    failed |= expect(fat_mkdir("APPS/EMPTY") == 0,
                     "create empty directory for removal");
    failed |= expect(fat_rmdir("APPS/EMPTY/.") != 0,
                     "reject removal through a dot entry");
    failed |= expect(fat_stat("APPS/EMPTY", &(struct fat_stat){0}) == 0,
                     "dot removal leaves directory intact");
    failed |= expect(fat_rmdir("APPS/EMPTY") == 0,
                     "remove empty directory");
    failed |= expect(fat_stat("APPS/EMPTY", &(struct fat_stat){0}) != 0,
                     "removed directory is absent");
    failed |= expect(fat_mkdir("APPS/Empty Long Directory") == 0,
                     "create long-name empty directory");
    failed |= expect(fat_rmdir("APPS/Empty Long Directory") == 0,
                     "remove long-name empty directory");
    failed |= expect(
        fat_stat("APPS/Empty Long Directory", &(struct fat_stat){0}) != 0,
        "removed long-name directory is absent");

    struct fat_file file;
    static const char payload[] = "nested directory data";
    failed |= expect(fat_create("APPS/TOOLS/NOTE.TXT", &file) == 0,
                     "create nested file");
    failed |= expect(fat_write(&file, payload, sizeof(payload))
                         == sizeof(payload),
                     "write nested file");

    memset(&file, 0, sizeof(file));
    failed |= expect(fat_open("apps\\tools\\note.txt", &file) == 0,
                     "open nested path case-insensitively");
    char readback[sizeof(payload)] = {0};
    failed |= expect(fat_read(&file, readback, sizeof(readback))
                         == sizeof(readback),
                     "read nested file");
    failed |= expect(memcmp(readback, payload, sizeof(payload)) == 0,
                     "nested file contents match");

    /* A 512-byte cluster holds 16 directory entries; two are . and ... */
    for (int i = 0; i < 20; i++) {
        char path[32];
        snprintf(path, sizeof(path), "APPS/TOOLS/F%02d.TXT", i);
        failed |= expect(fat_create(path, &file) == 0,
                         "grow directory across a cluster boundary");
    }
    failed |= expect(fat_open("APPS/TOOLS/F19.TXT", &file) == 0,
                     "find an entry in the grown directory cluster");
    failed |= expect(fat_rmdir("APPS/TOOLS") != 0,
                     "reject removal of non-empty directory");

    uint8_t big_payload[1600];
    for (size_t i = 0; i < sizeof(big_payload); i++)
        big_payload[i] = (uint8_t)(i * 17u + 3u);
    failed |= expect(fat_create("APPS/TOOLS/BIG.BIN", &file) == 0,
                     "create multi-cluster file");
    failed |= expect(fat_write(&file, big_payload, sizeof(big_payload))
                         == sizeof(big_payload),
                     "write multi-cluster file");
    failed |= expect(fat_seek(&file, 1024) == 0,
                     "seek to exact non-EOF cluster boundary");
    uint8_t boundary_read[32];
    failed |= expect(fat_read(&file, boundary_read, sizeof(boundary_read))
                         == sizeof(boundary_read),
                     "read after exact cluster-boundary seek");
    failed |= expect(memcmp(boundary_read, big_payload + 1024,
                            sizeof(boundary_read)) == 0,
                     "cluster-boundary seek selects the containing cluster");

    char names[128];
    uint32_t index = 0;
    long bytes = fat_read_dir("/", &index, names, sizeof(names));
    failed |= expect(bytes > 0 && contains_name(names, bytes, "APPS/"),
                     "enumerate root directory");
    index = 0;
    bytes = fat_read_dir("APPS", &index, names, sizeof(names));
    failed |= expect(bytes > 0 && contains_name(names, bytes, "TOOLS/"),
                     "enumerate first-level directory");
    index = 0;
    bytes = fat_read_dir("/APPS/TOOLS", &index, names, sizeof(names));
    failed |= expect(bytes > 0 && contains_name(names, bytes, "NOTE.TXT"),
                     "enumerate nested directory");

    failed |= expect(fat_create("APPS/NAME-IS-TOO-LONG.TXT", &file) == 0,
                     "create VFAT long-name file");
    failed |= expect(
        fat_stat("APPS/NAME-IS-TOO-LONG.TXT", &(struct fat_stat){0}) == 0,
        "stat VFAT long-name file");
    failed |= expect(fat_unlink("APPS/TOOLS/NOTE.TXT") == 0,
                     "unlink nested file");
    failed |= expect(fat_open("APPS/TOOLS/NOTE.TXT", &file) != 0,
                     "unlinked nested file is absent");

    free(image);
    if (failed)
        return 1;
    puts("fat_directory_test: PASS");
    return 0;
}
