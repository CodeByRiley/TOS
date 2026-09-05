/* Inspect an on-disk boot record without modifying the volume. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <lib/syscall.h>

static uint16_t le16(const unsigned char *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }
int main(int argc, char **argv) {
    unsigned char boot[512];
    if (argc != 2) { printf("usage: diskinfo DEVICE\n"); return 2; }
    if (blockdev_read(argv[1], 0, 1, boot) != 1) {
        printf("diskinfo: cannot read %s\n", argv[1]); return 1;
    }
    printf("device: %s\n", argv[1]);
    printf("signature: %s\n",
           boot[510] == 0x55 && boot[511] == 0xaa ? "0x55aa" : "absent");
    if (!memcmp(boot + 82, "FAT32", 5)) {
        printf("filesystem: FAT32\nsectors: %u\ncluster: %u bytes\n",
               le32(boot + 32), (unsigned)le16(boot + 11) * boot[13]);
    } else {
        /* ext2's superblock begins at byte 1024 (LBA 2), not in the boot
         * sector.  A missing 0x55aa is normal for an ext2 whole disk. */
        unsigned char super[512];
        if (blockdev_read(argv[1], 2, 1, super) == 1 && le16(super + 56) == 0xef53)
            printf("filesystem: ext2\n");
        else
            printf("filesystem: unknown or partitioned\n");
    }
    return 0;
}
