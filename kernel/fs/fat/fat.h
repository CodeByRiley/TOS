/* kernel/fs/fat/fat.h , FAT16/32 filesystem interface.
 *
 * Operates on an in-memory filesystem image and optionally writes changed
 * sectors through to a storage backend. BPB and directory-entry layouts stay
 * private so VFS and storage callers do not depend on disk-format structs.
 *
 * Path facade: fat.c. Volume/cluster state: fat_mount.c. Byte I/O:
 * fat_file.c. Directory entries and name encoding: fat_directory.c/fat_name.c.
 */
#ifndef FS_FAT_H
#define FS_FAT_H

#include <stddef.h>
#include <stdint.h>
#include <utilities/types.h>

#define FAT_PATH_MAX    260
#define FAT_LFN_MAX     255
#define FAT_NAME_LEN    11
#define FAT_DIRENT_MAX  (FAT_LFN_MAX + 2)

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20

typedef void (*fat_sector_writer)(uint32_t lba, const void *data);

struct fat_file {
  uint32_t first_cluster;
  uint32_t cur_cluster;
  uint32_t size;
  uint32_t pos;
  void *dir_ent;
};

struct fat_stat {
  uint32_t first_cluster;
  uint64_t size;
  uint8_t attr;
  int is_dir;
};

int fat_init(uint8_t *image, size_t size);
int fat_type_bits(void);
int fat_open(const char *path, struct fat_file *file);
int fat_create(const char *path, struct fat_file *file);
int fat_unlink(const char *path);
int fat_truncate(struct fat_file *file);
int fat_mkdir(const char *path);
int fat_rmdir(const char *path);
int fat_stat(const char *path, struct fat_stat *out);
size_t fat_read(struct fat_file *file, void *buffer, size_t length);
size_t fat_write(struct fat_file *file, const void *buffer, size_t length);
int fat_seek(struct fat_file *file, uint32_t position);
long fat_read_dir(const char *path, uint32_t *index, char *buffer,
                  size_t length);
long fat_read_root_dir(uint32_t *index, char *buffer, size_t length);
long fat_read_dir_one(const char *path, uint32_t *index, char *buffer,
                      size_t length, int *is_dir);
void fat_set_sector_writer(fat_sector_writer writer);
int fat_write_through_enabled(void);

/* Storage-backend helpers; neither exposes an on-disk structure. */
size_t fat_volume_size(const void *boot_sector,
                       uint32_t *bytes_per_sector_out);
uint8_t *fat_image_base(size_t *size_out,
                        uint32_t *bytes_per_sector_out);

#endif
