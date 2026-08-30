/* kernel/fs/probe.h , bounds-checked filesystem recognition helpers.
 *
 * Matches filesystem signatures without exposing on-disk backend structures
 * or reading beyond the supplied image. Geometry and feature validation stay
 * in the filesystem that owns the format.
 *
 * Implementation: header-only.
 */
#ifndef KERNEL_FS_PROBE_H
#define KERNEL_FS_PROBE_H

#include <fs/magic.h>
#include <stddef.h>
#include <stdint.h>

static inline int fs_probe_bytes_equal(const void *image, size_t size,
                                       size_t offset, const char *signature,
                                       size_t signature_size) {
  if (!image || !signature || offset > size || signature_size > size - offset)
    return 0;
  const uint8_t *bytes = image;
  for (size_t i = 0; i < signature_size; i++)
    if (bytes[offset + i] != (uint8_t)signature[i])
      return 0;
  return 1;
}

static inline int fs_probe_is_ntfs(const void *image, size_t size) {
  return fs_probe_bytes_equal(image, size, NTFS_OEM_ID_OFFSET,
                              NTFS_SIGNATURE, sizeof(NTFS_SIGNATURE) - 1);
}

static inline int fs_probe_is_exfat(const void *image, size_t size) {
  return fs_probe_bytes_equal(image, size, EXFAT_OEM_ID_OFFSET,
                              EXFAT_SIGNATURE, sizeof(EXFAT_SIGNATURE) - 1);
}

static inline int fs_probe_has_boot_signature(const void *image, size_t size) {
  if (!image || size < FS_BOOT_SECTOR_SIZE)
    return 0;
  const uint8_t *bytes = image;
  return bytes[FS_BOOT_SIGNATURE_OFFSET] == FS_BOOT_SIGNATURE_LOW &&
         bytes[FS_BOOT_SIGNATURE_OFFSET + 1] == FS_BOOT_SIGNATURE_HIGH;
}

static inline int fs_probe_has_fat_type(const void *image, size_t size,
                                        size_t offset) {
  return fs_probe_bytes_equal(image, size, offset, FAT_SIGNATURE,
                              sizeof(FAT_SIGNATURE) - 1);
}

static inline int fs_probe_is_fat12_or_16(const void *image, size_t size) {
  return fs_probe_has_boot_signature(image, size) &&
         fs_probe_has_fat_type(image, size, FAT12_TYPE_OFFSET);
}

static inline int fs_probe_is_fat32(const void *image, size_t size) {
  return fs_probe_has_boot_signature(image, size) &&
         fs_probe_has_fat_type(image, size, FAT32_TYPE_OFFSET);
}

static inline int fs_probe_is_ext(const void *image, size_t size) {
  if (!image || size < EXT_SUPERBLOCK_OFFSET + EXT_SUPERBLOCK_SIZE)
    return 0;
  const uint8_t *magic = (const uint8_t *)image + EXT_SUPERBLOCK_OFFSET +
                         EXT_MAGIC_FIELD_OFFSET;
  uint16_t value = (uint16_t)magic[0] | ((uint16_t)magic[1] << 8);
  return value == EXT_MAGIC;
}

#endif
