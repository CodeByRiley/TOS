/* kernel/fs/magic.h , filesystem signature constants.
 *
 * Collects disk-format identifiers used by filesystem probes without
 * exposing any backend-private on-disk structures.
 *
 * Implementation: header-only.
 */
#ifndef KERNEL_FS_MAGIC_H
#define KERNEL_FS_MAGIC_H

#include <stdint.h>
#include <utilities/string.h>

/* ext2, ext3, and ext4 share the same superblock signature. */
#define EXT_MAGIC       UINT16_C(0xEF53)

/* exFAT filesystem boot-region signature. */
#define EXFAT_MAGIC     UINT32_C(0x2011BAB0)

/* HFS family signatures. */
#define HFS_MAGIC UINT16_C(0x4244)
#define HFS_PLUS_MAGIC UINT16_C(0x482B)
#define HFSX_MAGIC UINT16_C(0x4858)

#define NTFS_SIGNATURE "NTFS    "
#define EXFAT_SIGNATURE "EXFAT   "

/*
 * FAT12/FAT16 and FAT32 store an informational filesystem-type string
 * at different offsets.
 */
#define FAT12_SIGNATURE "FAT"
#define FAT16_SIGNATURE "FAT"
#define FAT32_SIGNATURE "FAT"

#define NTFS_OEM_ID_OFFSET 3
#define EXFAT_OEM_ID_OFFSET 3

#define FAT12_TYPE_OFFSET 54
#define FAT16_TYPE_OFFSET 54
#define FAT32_TYPE_OFFSET 82

SINLINE int is_ntfs(const unsigned char *boot_sector) {
  return memcmp(boot_sector + NTFS_OEM_ID_OFFSET, NTFS_SIGNATURE,
                sizeof(NTFS_SIGNATURE) - 1) == 0;
}

SINLINE int is_exfat(const unsigned char *boot_sector) {
  return memcmp(boot_sector + EXFAT_OEM_ID_OFFSET, EXFAT_SIGNATURE,
                sizeof(EXFAT_SIGNATURE) - 1) == 0;
}
SINLINE int is_fat12_or_fat16(const unsigned char *boot_sector) {
  return memcmp(boot_sector + FAT12_TYPE_OFFSET, FAT12_SIGNATURE,
                sizeof(FAT12_SIGNATURE) - 1) == 0;
}

SINLINE int is_fat32(const unsigned char *boot_sector) {
  return memcmp(boot_sector + FAT32_TYPE_OFFSET, FAT32_SIGNATURE,
                sizeof(FAT32_SIGNATURE) - 1) == 0;
}
#endif
