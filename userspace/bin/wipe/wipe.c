/* Wipe a complete unmounted volume.  --all is an intentional safety latch. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <lib/syscall.h>
#define CHUNK 64u
int main(int argc, char **argv) {
    struct blockdev_info disks[64];
    if (argc != 3 || strcmp(argv[2], "--all")) {
        printf("usage: wipe DEVICE --all\n"); return 2;
    }
    long count = blockdev_list(disks, 64);
    uint64_t sectors = 0;
    for (long i = 0; i < count; i++) {
        disks[i].name[BLOCKDEV_NAME_MAX - 1] = 0;
        if (!strcmp(argv[1], disks[i].name) ||
            (!strncmp(argv[1], "/dev/", 5) && !strcmp(argv[1] + 5, disks[i].name)))
            sectors = disks[i].sectors;
    }
    if (!sectors) { printf("wipe: unknown device %s\n", argv[1]); return 1; }
    static unsigned char zero[CHUNK * 512];
    for (uint64_t lba = 0; lba < sectors; ) {
        uint32_t count_now = sectors - lba > CHUNK ? CHUNK : (uint32_t)(sectors - lba);
        if (blockdev_write(argv[1], lba, count_now, zero) != count_now) {
            printf("wipe: write refused or failed at sector %llu\n", (unsigned long long)lba);
            return 1;
        }
        lba += count_now;
    }
    if (blockdev_flush(argv[1])) { printf("wipe: flush failed\n"); return 1; }
    printf("wipe: erased %llu sectors on %s\n", (unsigned long long)sectors, argv[1]);
    return 0;
}
