/* kernel/fs/magic.h , filesystem signature constants and offsets.
 *
 * Collects disk-format identifiers and their locations without exposing any
 * backend-private on-disk structures. Bounds-checked recognition helpers live
 * in kernel/fs/probe.h.
 *
 * Implementation: header-only.
 */
#ifndef KERNEL_FS_MAGIC_H
#define KERNEL_FS_MAGIC_H

#include <stdint.h>

/* ext2, ext3, and ext4 share the same superblock signature. */
#define EXT_MAGIC                   UINT16_C(0xEF53)

/* exFAT filesystem boot-region signature. */
#define EXFAT_MAGIC                 UINT32_C(0x2011BAB0)

/* HFS family signatures. */
#define HFS_MAGIC                   UINT16_C(0x4244)
#define HFS_PLUS_MAGIC              UINT16_C(0x482B)
#define HFSX_MAGIC                  UINT16_C(0x4858)

#define NTFS_SIGNATURE              "NTFS    "
#define EXFAT_SIGNATURE             "EXFAT   "

/*
 * FAT12/FAT16 and FAT32 store an informational filesystem-type string
 * at different offsets.
 */
#define FAT_SIGNATURE               "FAT"

#define NTFS_OEM_ID_OFFSET          3
#define EXFAT_OEM_ID_OFFSET         3
#define FAT12_TYPE_OFFSET           54
#define FAT16_TYPE_OFFSET           54
#define FAT32_TYPE_OFFSET           82
#define FS_BOOT_SECTOR_SIZE         512
#define FS_BOOT_SIGNATURE_OFFSET    510
#define FS_BOOT_SIGNATURE_LOW       UINT8_C(0x55)
#define FS_BOOT_SIGNATURE_HIGH      UINT8_C(0xAA)

#define EXT_SUPERBLOCK_OFFSET       1024
#define EXT_SUPERBLOCK_SIZE         1024
#define EXT_MAGIC_FIELD_OFFSET      56

#endif
