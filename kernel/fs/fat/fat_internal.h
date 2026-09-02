/* FAT-private layouts and shared helpers. Kernel callers use the VFS. */
#ifndef FS_FAT_INTERNAL_H
#define FS_FAT_INTERNAL_H

#include "fat.h"
#include <utilities/types.h>

/* Private directory-entry flags. */
#define FAT_ATTR_LFN 0x0F
#define FAT_CASE_BASE_LOWER 0x08
#define FAT_CASE_EXT_LOWER 0x10
#define FAT_LFN_CHARS_PER_SLOT 13
#define FAT_LFN_MAX_SLOTS \
  ((FAT_LFN_MAX + FAT_LFN_CHARS_PER_SLOT - 1) / FAT_LFN_CHARS_PER_SLOT)
#define FAT_LFN_BUFFER (FAT_LFN_MAX_SLOTS * FAT_LFN_CHARS_PER_SLOT + 1)
#define FAT_MAX_ENTRY_SLOTS (FAT_LFN_MAX_SLOTS + 1)

/* On-disk 32-byte directory entry. */
struct PACKED dir_entry {
  char name[8];
  char ext[3];
  u8 attr;
  u8 nt_case;
  u8 create_time_tenth;
  u16 create_time;
  u16 create_date;
  u16 access_date;
  u16 first_cluster_high;
  u16 write_time;
  u16 write_date;
  u16 first_cluster_low;
  u32 size;
};
_Static_assert(sizeof(struct dir_entry) == 32,
               "FAT directory entries must be 32 bytes");

/* Long-filename accumulator. */
struct lfn_state {
  char name[FAT_LFN_BUFFER];
  u32 length;
  u32 start_index;
  u8 checksum;
  u8 expect;
  u8 valid;
};

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

/* One retained FAT volume, with geometry and persistence kept together.
 * The VFS rejects a second mount until the disk engine takes a context. */
struct fat_volume {
  u8 *image;
  usize image_size;
  u16 bytes_per_sector;
  u8 sectors_per_cluster, fat_count;
  u32 sectors_per_fat, fat_start_sector, root_start_sector, data_start_sector;
  u16 root_entries;
  u32 root_cluster, cluster_limit;
  enum fat_type type;
  fat_sector_writer write_sector;
};
extern struct fat_volume fat_volume;

/* Component operations shared by the VFS and legacy FAT test facade. */
int fat_create_at(struct fat_dir parent, const char *name, struct found_entry *out);
int fat_mkdir_at(struct fat_dir parent, const char *name);
int fat_remove_at(struct fat_dir parent, const char *name, int is_directory);
void fat_file_from_entry(void *entry, struct fat_file *file);

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

/* Shared directory and allocation primitives for both FAT layouts. */
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


/* Shared VFAT name codec; collision scanning remains in the directory engine. */
u8 lfn_checksum(const char *name);
void lfn_reset(struct lfn_state *state);
void lfn_feed(struct lfn_state *state, const struct dir_entry *entry, u32 index);
const char *lfn_take(struct lfn_state *state, const struct dir_entry *entry);
void write_lfn_slot(struct dir_entry *slot, const char *name, u32 length,
                    u8 seq, int last, u8 checksum);
int short_name_exact(const char *name, u32 length, char out[11], u8 *nt_case);
int short_name_alias(struct fat_dir dir, const char *name, u32 length, char out[11]);
u32 entry_short_name(const struct dir_entry *entry, char *out);
int short_name_taken(struct fat_dir dir, const char name[11]);
#endif
