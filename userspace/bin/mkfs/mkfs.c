/* Create the FAT32 layout supported by TOS on an unmounted whole volume. */
#include <lib/syscall.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SECTOR 512u
#define RESERVED 32u
#define FATS 2u
#define CHUNK 64u
static void put16(unsigned char *p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}
static void put32(unsigned char *p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}
static uint64_t find_size(const char *name) {
  struct blockdev_info disks[64];
  long n = blockdev_list(disks, 64);
  for (long i = 0; i < n; i++) {
    disks[i].name[15] = 0;
    if (!strcmp(name, disks[i].name) ||
        (!strncmp(name, "/dev/", 5) && !strcmp(name + 5, disks[i].name)))
      return disks[i].sectors;
  }
  return 0;
}
static int write_one(const char *dev, uint64_t lba, const void *p) {
  return blockdev_write(dev, lba, 1, p) == 1 ? 0 : -1;
}
int main(int argc, char **argv) {
  if (argc != 3 || strcmp(argv[2], "fat32")) {
    printf("usage: mkfs DEVICE fat32\n");
    return 2;
  }
  uint64_t total = find_size(argv[1]);
  if (total > UINT32_MAX || total < 65525u + RESERVED) {
    printf("mkfs: FAT32 needs a 32 MiB..2 TiB device\n");
    return 1;
  }
  uint32_t fat = 1;
  for (;;) {
    uint64_t clusters = total - RESERVED - FATS * fat;
    uint32_t need = (uint32_t)(((clusters + 2) * 4 + SECTOR - 1) / SECTOR);
    if (need == fat)
      break;
    fat = need;
  }
  uint64_t clusters = total - RESERVED - FATS * fat;
  if (clusters < 65525) {
    printf("mkfs: device is too small for FAT32\n");
    return 1;
  }
  static unsigned char zero[CHUNK * SECTOR], boot[SECTOR], fsinfo[SECTOR],
      table[SECTOR];
  /* Clear old allocation data and file contents before publishing a boot
   * sector. */
  for (uint64_t lba = 0; lba < total;) {
    uint32_t n = total - lba > CHUNK ? CHUNK : (uint32_t)(total - lba);
    if (blockdev_write(argv[1], lba, n, zero) != n) {
      printf("mkfs: write refused or failed\n");
      return 1;
    }
    lba += n;
  }
  boot[0] = 0xeb;
  boot[1] = 0x58;
  boot[2] = 0x90;
  memcpy(boot + 3, "TOSFAT32", 8);
  put16(boot + 11, SECTOR);
  boot[13] = 1;
  put16(boot + 14, RESERVED);
  boot[16] = FATS;
  boot[21] = 0xf8;
  put16(boot + 24, 63);
  put16(boot + 26, 255);
  put32(boot + 32, (uint32_t)total);
  put32(boot + 36, fat);
  put32(boot + 44, 2);
  put16(boot + 48, 1);
  put16(boot + 50, 6);
  boot[64] = 0x80;
  boot[66] = 0x29;
  put32(boot + 67, 0x544f5331);
  memcpy(boot + 71, "TOSDISK    FAT32   ", 19);
  boot[510] = 0x55;
  boot[511] = 0xaa;
  put32(fsinfo, 0x41615252);
  put32(fsinfo + 484, 0x61417272);
  put32(fsinfo + 488, (uint32_t)(clusters - 1));
  put32(fsinfo + 492, 3);
  put32(fsinfo + 508, 0xaa550000);
  put32(table, 0x0ffffff8);
  put32(table + 4, 0x0fffffff);
  put32(table + 8, 0x0fffffff);
  if (write_one(argv[1], 0, boot) || write_one(argv[1], 6, boot) ||
      write_one(argv[1], 1, fsinfo) || write_one(argv[1], 7, fsinfo) ||
      write_one(argv[1], RESERVED, table) ||
      write_one(argv[1], RESERVED + fat, table) || blockdev_flush(argv[1])) {
    printf("mkfs: metadata write failed\n");
    return 1;
  }
  printf("mkfs: created FAT32 on %s (%llu sectors)\n", argv[1],
         (unsigned long long)total);
  return 0;
}
