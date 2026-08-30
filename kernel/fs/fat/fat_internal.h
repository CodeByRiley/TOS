/* kernel/fs/fat/fat_internal.h , private FAT16/32 backend interface.
 *
 * Shares mount state, path helpers, and the backend operation table between
 * the common FAT facade, the FAT16/FAT32 selectors, and the VFAT directory
 * engine. External callers include fat.h instead.
 *
 * Implementation: kernel/fs/fat/fat.c,
 *                 kernel/fs/fat/fat16.c,
 *                 kernel/fs/fat/fat32.c,
 *                 kernel/fs/fat/fat_directory.c.
 */
#ifndef FS_FAT_INTERNAL_H
#define FS_FAT_INTERNAL_H

#include "fat.h"
#include <utilities/types.h>

/* Mounted FAT format. */
enum fat_type {
  FAT_TYPE_NONE = 0,
  FAT_TYPE_16 = 16,
  FAT_TYPE_32 = 32,
};

/* Directory location within the mounted image. */
struct fat_dir {
  u32 first_cluster;
  int is_root;
};

/* Backend-private result returned by directory lookup and creation. */
struct found_entry {
  u32 first_cluster;
  u64 size;
  u8 attr;
  u32 index;
  u32 index_end;
  void *raw;
};

/* Operations supplied by each FAT format backend. */
struct fat_ops {
  /* Mount setup. */
  int (*init)(u8 *image, usize size);

  /* Cluster allocation. */
  u32 (*alloc_cluster)(void);
  void (*free_chain)(u32 first);

  /* Directory operations. */
  int (*find_entry)(struct fat_dir dir, const char *name,
                    struct found_entry *out);
  int (*create_entry)(struct fat_dir parent, const char *name,
                      u8 attr, u32 cluster, struct found_entry *out);
  void (*erase_entry)(struct fat_dir dir, struct found_entry *entry);
  int (*init_dir_cluster)(u32 cluster, struct fat_dir parent);
  int (*read_dir)(struct fat_dir dir, u32 *index,
                  char *buffer, usize length);
  int (*dir_is_empty)(u32 cluster);
  long (*read_dir_one)(struct fat_dir dir, u32 *index,
                       char *buffer, usize length, int *is_dir);

  /* Directory-entry fields. */
  u32 (*entry_get_cluster)(void *entry);
  void (*entry_set_cluster)(void *entry, u32 cluster);
  u64 (*entry_get_size)(void *entry);
  void (*entry_set_size)(void *entry, u64 size);

  /* Directory-entry timestamps. */
  void (*set_timestamp)(void *entry);
};

/* State shared by the facade and format backends. */
extern u8 *fs_image;
extern usize fs_image_size;
extern u16 bytes_per_sec;
extern u8 sec_per_clus;
extern u8 num_fats;
extern u32 sectors_per_fat;
extern u32 fat_start_sec;
extern u32 root_start_sec;
extern u32 data_start_sec;
extern u16 root_entries;
extern u32 root_cluster;
extern enum fat_type fat_type;
extern u32 cluster_limit;
extern fat_sector_writer sector_writer;

/* Backend selected by fat_init. */
extern const struct fat_ops *fs_ops;

/* Cluster and image helpers shared by the format backends. */
u32 cluster_bytes(void);
u8 *sector(u32 lba);
int cluster_is_valid(u32 cluster);
u32 cluster_eoc(void);
int cluster_is_eoc(u32 value);
u32 fat_get(u32 cluster);
void fat_set(u32 cluster, u32 value);
int next_cluster(u32 cluster, u32 *next);
u8 *cluster_data(u32 cluster);
void fat_flush_sector(u32 lba);
void fat_flush_bytes(const void *start, usize len);
struct fat_dir root_directory(void);
int dir_is_fixed_root(struct fat_dir dir);

/* Parsed path components. */
struct path_parts {
  const char *component[16];
  u8 length[16];
  u8 count;
};
int split_path(const char *path, struct path_parts *parts);
int walk_directories(const struct path_parts *parts, u32 count,
                     struct fat_dir *out);
int resolve_parent(const char *path, struct fat_dir *parent,
                   char leaf[FAT_LFN_MAX + 1]);

/* Shared BPB validation used by the small format-specific backends. */
int fat_mount_format(u8 *image, usize size, enum fat_type expected_type);

/* Shared VFAT implementation used by both format operation tables. */
u32 fat_impl_alloc_cluster(void);
void fat_impl_free_chain(u32 first);
int fat_impl_find_entry(struct fat_dir dir, const char *name,
                        struct found_entry *out);
int fat_impl_create_entry(struct fat_dir parent, const char *name,
                          u8 attr, u32 cluster, struct found_entry *out);
void fat_impl_erase_entry(struct fat_dir dir, struct found_entry *entry);
int fat_impl_init_dir_cluster(u32 cluster, struct fat_dir parent);
int fat_impl_dir_is_empty(u32 cluster);
int fat_impl_read_dir(struct fat_dir dir, u32 *index,
                      char *buffer, usize length);
long fat_impl_read_dir_one(struct fat_dir dir, u32 *index,
                           char *buffer, usize length, int *is_dir);
u32 fat_impl_entry_get_cluster(void *entry);
void fat_impl_entry_set_cluster(void *entry, u32 cluster);
u64 fat_impl_entry_get_size(void *entry);
void fat_impl_entry_set_size(void *entry, u64 size);
void fat_impl_set_timestamp(void *entry);

extern const struct fat_ops fat16_ops;
extern const struct fat_ops fat32_ops;

#endif
