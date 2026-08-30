/* kernel/fs/fat/fat.c , shared FAT16/32 facade and cluster logic.
 *
 * Owns the mounted-image state, cluster-chain primitives, path walking, and
 * public file API. Format selection and directory-entry encoding live in the
 * smaller backend translation units.
 */
#include "fat_internal.h"
#include <fs/probe.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* State for the single mounted FAT image. */
u8 *fs_image;
usize fs_image_size;
u16 bytes_per_sec;
u8 sec_per_clus;
u8 num_fats;
u32 sectors_per_fat;
u32 fat_start_sec;
u32 root_start_sec;
u32 data_start_sec;
u16 root_entries;
u32 root_cluster;
enum fat_type fat_type;
u32 cluster_limit;
fat_sector_writer sector_writer;
const struct fat_ops *fs_ops;

/* Cluster helpers shared by both formats. */
u32 cluster_bytes(void) {
  return (u32)sec_per_clus * bytes_per_sec;
}

u8 *sector(u32 lba) {
  return fs_image + (u64)lba * bytes_per_sec;
}

int cluster_is_valid(u32 cluster) {
  return cluster >= 2 && cluster < cluster_limit;
}

u32 cluster_eoc(void) {
  switch (fat_type) {
    case FAT_TYPE_32:  return 0x0FFFFFFFu;
    default:           return 0xFFFFu;
  }
}

int cluster_is_eoc(u32 value) {
  switch (fat_type) {
    case FAT_TYPE_32:  return value >= 0x0FFFFFF8u;
    default:           return value >= 0xFFF8u;
  }
}

u32 fat_get(u32 cluster) {
  u8 *table = sector(fat_start_sec);
  switch (fat_type) {
    case FAT_TYPE_32:
      return ((u32 *)table)[cluster] & 0x0FFFFFFFu;
    default:
      return ((u16 *)table)[cluster];
  }
}

static void fat_put(u8 *table, u32 cluster, u32 value) {
  switch (fat_type) {
    case FAT_TYPE_32: {
      u32 *slots = (u32 *)table;
      slots[cluster] = (slots[cluster] & 0xF0000000u) | (value & 0x0FFFFFFFu);
      break;
    }
    default:
      ((u16 *)table)[cluster] = (u16)value;
      break;
  }
}

void fat_set(u32 cluster, u32 value) {
  usize entry_width = (fat_type == FAT_TYPE_16) ? 2u : 4u;
  for (u8 f = 0; f < num_fats; f++) {
    u8 *table = sector(fat_start_sec + f * sectors_per_fat);
    fat_put(table, cluster, value);
    fat_flush_bytes(table + (usize)cluster * entry_width, entry_width);
  }
}

int fat_type_bits(void) {
  return (int)fat_type;
}

void fat_set_sector_writer(fat_sector_writer writer) {
  sector_writer = writer;
}

static u16 read_le16(const u8 *bytes) {
  return (u16)bytes[0] | ((u16)bytes[1] << 8);
}

static u32 read_le32(const u8 *bytes) {
  return (u32)bytes[0] | ((u32)bytes[1] << 8) |
         ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
}

usize fat_volume_size(const void *boot_sector, u32 *bytes_per_sector_out) {
  if (bytes_per_sector_out)
    *bytes_per_sector_out = 0;
  if (!boot_sector)
    return 0;

  /* exFAT has a different boot layout and must not be treated as FAT32. */
  if (fs_probe_is_exfat(boot_sector, FS_BOOT_SECTOR_SIZE))
    return 0;

  const u8 *boot = boot_sector;

  u32 bytes_per_sector = read_le16(boot + 11);
  u32 total_sectors = read_le16(boot + 19);
  if (!total_sectors)
    total_sectors = read_le32(boot + 32);
  if (bytes_per_sector < 512 || bytes_per_sector > 4096 ||
      (bytes_per_sector & (bytes_per_sector - 1)) != 0 || !total_sectors)
    return 0;

  if (bytes_per_sector_out)
    *bytes_per_sector_out = bytes_per_sector;
  return (usize)total_sectors * bytes_per_sector;
}

u8 *fat_image_base(usize *size_out, u32 *bytes_per_sector_out) {
  if (size_out)
    *size_out = fs_image ? fs_image_size : 0;
  if (bytes_per_sector_out)
    *bytes_per_sector_out = fs_image ? bytes_per_sec : 0;
  return fs_image;
}

long fat_read_root_dir(u32 *index, char *buffer, usize length) {
  return fat_read_dir("/", index, buffer, length);
}

int fat_stat(const char *path, struct fat_stat *out) {
  if (!out)
    return -1;

  /* The root directory has no on-disk directory entry. */
  struct path_parts parts;
  if (split_path(path, &parts) == 0 && parts.count == 0) {
    out->size = 0;
    out->first_cluster = root_cluster;
    out->attr = FAT_ATTR_DIRECTORY;
    out->is_dir = 1;
    return 0;
  }

  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0)
    return -1;

  struct found_entry found;
  if (fs_ops->find_entry(parent, leaf, &found) != 0)
    return -1;

  out->first_cluster = found.first_cluster;
  out->size = found.size;
  out->attr = found.attr;
  out->is_dir = (found.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
  return 0;
}

static int path_ends_with_dot_component(const char *path) {
  if (!path)
    return 1;
  u32 length = 0;
  while (length < FAT_PATH_MAX && path[length])
    length++;
  if (length == FAT_PATH_MAX)
    return 1;
  while (length > 0 &&
         (path[length - 1] == '/' || path[length - 1] == '\\'))
    length--;
  u32 start = length;
  while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\')
    start--;
  u32 component_length = length - start;
  return (component_length == 1 && path[start] == '.') ||
         (component_length == 2 && path[start] == '.' &&
          path[start + 1] == '.');
}

int fat_rmdir(const char *path) {
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (path_ends_with_dot_component(path) ||
      resolve_parent(path, &parent, leaf) != 0)
    return -1;

  struct found_entry found;
  if (fs_ops->find_entry(parent, leaf, &found) != 0)
    return -1;
  if (!(found.attr & FAT_ATTR_DIRECTORY))
    return -1;

  if (fs_ops->dir_is_empty &&
      fs_ops->dir_is_empty(found.first_cluster) != 0)
    return -1;

  if (found.first_cluster)
    fs_ops->free_chain(found.first_cluster);
  fs_ops->erase_entry(parent, &found);
  return 0;
}

long fat_read_dir_one(const char *path, u32 *index, char *buffer,
                      usize length, int *is_dir) {
  struct fat_dir dir;
  struct path_parts parts;
  if (split_path(path, &parts) != 0) return -1;
  if (walk_directories(&parts, parts.count, &dir) != 0) return -1;

  if (!fs_ops->read_dir_one)
    return -1;
  return fs_ops->read_dir_one(dir, index, buffer, length, is_dir);
}

int next_cluster(u32 cluster, u32 *next) {
  if (!cluster_is_valid(cluster))
    return -1;
  u32 value = fat_get(cluster);
  if (cluster_is_eoc(value))
    return 1;
  if (!cluster_is_valid(value))
    return -1;
  *next = value;
  return 0;
}

u8 *cluster_data(u32 cluster) {
  if (!cluster_is_valid(cluster))
    return 0;
  return sector(data_start_sec + (cluster - 2) * sec_per_clus);
}

void fat_flush_sector(u32 lba) {
  if (fs_image && sector_writer)
    sector_writer(lba, sector(lba));
}

void fat_flush_bytes(const void *start, usize len) {
  if (!fs_image || !sector_writer || !start || len == 0 || !bytes_per_sec)
    return;

  u64 base = (u64)(uintptr_t)fs_image;
  u64 at = (u64)(uintptr_t)start;
  if (at < base || at >= base + fs_image_size)
    return;

  u64 off = at - base;
  if (len > fs_image_size - off)
    len = (usize)(fs_image_size - off);

  u32 first = (u32)(off / bytes_per_sec);
  u32 last = (u32)((off + len - 1) / bytes_per_sec);
  for (u32 lba = first; lba <= last; lba++)
    fat_flush_sector(lba);
}

struct fat_dir root_directory(void) {
  struct fat_dir dir = { .is_root = 1, .first_cluster = 0 };
  if (fat_type != FAT_TYPE_16)
    dir.first_cluster = root_cluster;
  return dir;
}

int dir_is_fixed_root(struct fat_dir dir) {
  return dir.is_root && fat_type == FAT_TYPE_16;
}

/* Path parsing shared by FAT16 and FAT32. */
static int is_separator(char c) { return c == '/' || c == '\\'; }

int split_path(const char *path, struct path_parts *parts) {
  if (!path || !parts)
    return -1;
  parts->count = 0;

  u32 pos = 0;
  for (;;) {
    while (pos < FAT_PATH_MAX && is_separator(path[pos]))
      pos++;
    if (pos >= FAT_PATH_MAX)
      return -1;
    if (path[pos] == '\0')
      return 0;

    u32 start = pos;
    while (pos < FAT_PATH_MAX && path[pos] && !is_separator(path[pos]))
      pos++;
    if (pos >= FAT_PATH_MAX)
      return -1;

    u32 length = pos - start;
    if (length == 1 && path[start] == '.')
      continue;
    if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
      if (parts->count > 0) parts->count--;
      continue;
    }
    if (length == 0 || length > FAT_LFN_MAX || parts->count >= 16)
      return -1;

    parts->component[parts->count] = path + start;
    parts->length[parts->count] = (u8)length;
    parts->count++;
  }
}

int walk_directories(const struct path_parts *parts, u32 count,
                     struct fat_dir *out) {
  struct fat_dir current = root_directory();
  for (u32 i = 0; i < count; i++) {
    char component[FAT_LFN_MAX + 1];
    memcpy(component, parts->component[i], parts->length[i]);
    component[parts->length[i]] = '\0';

    struct found_entry found;
    if (fs_ops->find_entry(current, component, &found) != 0)
      return -1;
    if (!(found.attr & FAT_ATTR_DIRECTORY))
      return -1;

    if (found.first_cluster == 0) {
      current = root_directory();
    } else if (cluster_is_valid(found.first_cluster)) {
      current = (struct fat_dir){ .first_cluster = found.first_cluster, .is_root = 0 };
    } else {
      return -1;
    }
  }
  *out = current;
  return 0;
}

int resolve_parent(const char *path, struct fat_dir *parent,
                   char leaf[FAT_LFN_MAX + 1]) {
  struct path_parts parts;
  if (split_path(path, &parts) != 0 || parts.count == 0)
    return -1;
  u32 last = parts.count - 1;
  memcpy(leaf, parts.component[last], parts.length[last]);
  leaf[parts.length[last]] = '\0';
  return walk_directories(&parts, last, parent);
}

int fat_init(u8 *image, usize size) {
  if (!image || size < 512)
    return -1;
  if (fs_probe_is_exfat(image, size))
    return -1;

  fs_ops = &fat16_ops;
  if (fs_ops->init(image, size) == 0)
    return 0;
  fs_ops = &fat32_ops;
  if (fs_ops->init(image, size) == 0)
    return 0;
  fs_ops = 0;
  return -1;
}

/* Format-neutral public file API. */

int fat_open(const char *path, struct fat_file *file) {
  if (!file) return -1;
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0) return -1;

  struct found_entry found;
  if (fs_ops->find_entry(parent, leaf, &found) != 0) return -1;
  if (found.attr & FAT_ATTR_DIRECTORY) return -1;
  if (found.size != 0 && !cluster_is_valid(found.first_cluster)) return -1;

  file->first_cluster = found.first_cluster;
  file->cur_cluster = found.first_cluster;
  file->size = (u32)found.size;
  file->pos = 0;
  file->dir_ent = found.raw;
  return 0;
}

int fat_create(const char *path, struct fat_file *file) {
  if (!file) return -1;
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0) return -1;

  struct found_entry existing;
  if (fs_ops->find_entry(parent, leaf, &existing) == 0) return -1;

  struct found_entry new_entry;
  if (fs_ops->create_entry(parent, leaf, FAT_ATTR_ARCHIVE, 0, &new_entry) != 0)
    return -1;

  file->first_cluster = 0;
  file->cur_cluster = 0;
  file->size = 0;
  file->pos = 0;
  file->dir_ent = new_entry.raw;
  return 0;
}

int fat_unlink(const char *path) {
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0) return -1;

  struct found_entry found;
  if (fs_ops->find_entry(parent, leaf, &found) != 0) return -1;
  if (found.attr & FAT_ATTR_DIRECTORY) return -1;

  if (found.first_cluster)
    fs_ops->free_chain(found.first_cluster);
  fs_ops->erase_entry(parent, &found);
  return 0;
}

int fat_truncate(struct fat_file *file) {
  if (!file || !file->dir_ent) return -1;
  if (file->first_cluster)
    fs_ops->free_chain(file->first_cluster);
  file->first_cluster = 0;
  file->cur_cluster = 0;
  file->size = 0;
  file->pos = 0;
  fs_ops->entry_set_cluster(file->dir_ent, 0);
  fs_ops->entry_set_size(file->dir_ent, 0);
  fs_ops->set_timestamp(file->dir_ent);
  fat_flush_bytes(file->dir_ent, 32);
  return 0;
}

int fat_mkdir(const char *path) {
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0) return -1;

  struct found_entry existing;
  if (fs_ops->find_entry(parent, leaf, &existing) == 0) return -1;

  u32 cluster = fs_ops->alloc_cluster();
  if (!cluster) return -1;

  /* Initialize the format-specific "." and ".." entries. */
  if (fs_ops->init_dir_cluster(cluster, parent) != 0) {
    fs_ops->free_chain(cluster);
    return -1;
  }

  struct found_entry new_entry;
  if (fs_ops->create_entry(parent, leaf, FAT_ATTR_DIRECTORY, cluster, &new_entry) != 0) {
    fs_ops->free_chain(cluster);
    return -1;
  }
  return 0;
}

/* Read data from an open file. */
usize fat_read(struct fat_file *file, void *buffer, usize length) {
  if (!file || !buffer)
    return 0;
  u8 *out = (u8 *)buffer;
  usize total = 0;
  u32 bytes = cluster_bytes();

  while (length > 0 && file->pos < file->size) {
    u8 *data = cluster_data(file->cur_cluster);
    if (!data)
      break;

    /* Limit the transfer to this cluster and the remaining file data. */
    u32 offset = file->pos % bytes;
    u32 in_cluster = bytes - offset;
    u32 in_file = file->size - file->pos;
    usize chunk = length;
    if (chunk > in_cluster)
      chunk = in_cluster;
    if (chunk > in_file)
      chunk = in_file;

    memcpy(out, data + offset, chunk);
    out += chunk;
    file->pos += (u32)chunk;
    length -= chunk;
    total += chunk;

    /* Advance after consuming a complete cluster. */
    if (file->pos % bytes == 0 && file->pos < file->size) {
      u32 next;
      if (next_cluster(file->cur_cluster, &next) != 0)
        break;
      file->cur_cluster = next;
    }
  }
  return total;
}

/* Seek to a position in a file. */
int fat_seek(struct fat_file *file, u32 position) {
  if (!file)
    return -1;
  if (position > file->size)
    position = file->size;
  file->cur_cluster = file->first_cluster;
  if (position == 0 || file->size == 0) {
    file->pos = position;
    return 0;
  }

  u32 bytes = cluster_bytes();
  u32 skip = position / bytes;
  /* At an exact-boundary EOF, retain the preceding cluster for appends. */
  if (position == file->size && position % bytes == 0)
    skip--;

  /* Walk the chain to the cluster containing the new position. */
  while (skip--) {
    u32 next;
    if (next_cluster(file->cur_cluster, &next) != 0)
      return -1;
    file->cur_cluster = next;
  }
  file->pos = position;
  return 0;
}

usize fat_write(struct fat_file *file, const void *buffer, usize length) {
  if (!file || !file->dir_ent || !buffer)
    return 0;
  const u8 *in = (const u8 *)buffer;
  usize total = 0;
  u32 bytes = cluster_bytes();

  if (file->first_cluster == 0) {
    u32 cluster = fs_ops->alloc_cluster();
    if (!cluster)
      return 0;
    file->first_cluster = cluster;
    file->cur_cluster = cluster;
    fs_ops->entry_set_cluster(file->dir_ent, cluster);
  }

  /* Allocate or select the next cluster for a boundary append. */
  if (length > 0 && file->pos > 0 && file->pos == file->size &&
      file->pos % bytes == 0) {
    u32 next;
    int status = next_cluster(file->cur_cluster, &next);
    if (status == 1) {
      next = fs_ops->alloc_cluster();
      if (!next)
        return 0;
      fat_set(file->cur_cluster, next);
    } else if (status < 0) {
      return 0;
    }
    file->cur_cluster = next;
  }

  /* Write data one cluster at a time. */
  while (length > 0) {
    u8 *data = cluster_data(file->cur_cluster);
    if (!data)
      break;
    u32 offset = file->pos % bytes;
    u32 available = bytes - offset;
    usize chunk = length < available ? length : available;
    memcpy(data + offset, in, chunk);
    fat_flush_bytes(data + offset, chunk);

    in += chunk;
    file->pos += (u32)chunk;
    length -= chunk;
    total += chunk;

    /* Advance or extend the chain when this cluster is full. */
    if (file->pos % bytes == 0 && length > 0) {
      u32 next;
      int status = next_cluster(file->cur_cluster, &next);
      if (status == 1) {
        next = fs_ops->alloc_cluster();
        if (!next)
          break;
        fat_set(file->cur_cluster, next);
      } else if (status < 0) {
        break;
      }
      file->cur_cluster = next;
    }
  }

  /* Publish the new size in the directory entry. */
  if (file->pos > file->size) {
    file->size = file->pos;
    fs_ops->entry_set_size(file->dir_ent, file->size);
  }
  fs_ops->set_timestamp(file->dir_ent);
  fat_flush_bytes(file->dir_ent, 32);

  /* cur_cluster always contains the byte at pos while pos is inside the file. */
  if (file->pos > 0 && file->pos < file->size && file->pos % bytes == 0) {
    u32 next;
    if (next_cluster(file->cur_cluster, &next) == 0)
      file->cur_cluster = next;
  }
  return total;
}
long fat_read_dir(const char *path, u32 *index, char *buffer, usize length) {
  struct fat_dir dir;
  struct path_parts parts;
  if (split_path(path, &parts) != 0) return -1;
  if (walk_directories(&parts, parts.count, &dir) != 0) return -1;
  return fs_ops->read_dir(dir, index, buffer, length);
}
