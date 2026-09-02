/* Legacy path API and component mutations shared with the VFS adapter. */
#include "fat_internal.h"
#include <utilities/string.h>

/* Parsed path components. */
struct path_parts {
  const char *component[16];
  u8 length[16];
  u8 count;
};
static int split_path(const char *path, struct path_parts *parts);
static int walk_directories(const struct path_parts *parts, u32 count,
                     struct fat_dir *out);
static int resolve_parent(const char *path, struct fat_dir *parent,
                   char leaf[FAT_LFN_MAX + 1]);


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
    out->first_cluster = fat_volume.root_cluster;
    out->attr = FAT_ATTR_DIRECTORY;
    out->is_dir = 1;
    return 0;
  }

  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0)
    return -1;

  struct found_entry found;
  if (fat_impl_find_entry(parent, leaf, &found) != 0)
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
  char name[FAT_LFN_MAX + 1];
  if (path_ends_with_dot_component(path) || resolve_parent(path, &parent, name))
    return -1;
  return fat_remove_at(parent, name, 1);
}

long fat_read_dir_one(const char *path, u32 *index, char *buffer,
                      usize length, int *is_dir) {
  struct fat_dir dir;
  struct path_parts parts;
  if (split_path(path, &parts) != 0) return -1;
  if (walk_directories(&parts, parts.count, &dir) != 0) return -1;

  return fat_impl_read_dir_one(dir, index, buffer, length, is_dir);
}

/* Path parsing shared by FAT16 and FAT32. */
static int is_separator(char c) { return c == '/' || c == '\\'; }

static int split_path(const char *path, struct path_parts *parts) {
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

static int walk_directories(const struct path_parts *parts, u32 count,
                     struct fat_dir *out) {
  struct fat_dir current = root_directory();
  for (u32 i = 0; i < count; i++) {
    char component[FAT_LFN_MAX + 1];
    memcpy(component, parts->component[i], parts->length[i]);
    component[parts->length[i]] = '\0';

    struct found_entry found;
    if (fat_impl_find_entry(current, component, &found) != 0)
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

static int resolve_parent(const char *path, struct fat_dir *parent,
                   char leaf[FAT_LFN_MAX + 1]) {
  struct path_parts parts;
  if (split_path(path, &parts) != 0 || parts.count == 0)
    return -1;
  u32 last = parts.count - 1;
  memcpy(leaf, parts.component[last], parts.length[last]);
  leaf[parts.length[last]] = '\0';
  return walk_directories(&parts, last, parent);
}

/* Format-neutral public file API. */

int fat_open(const char *path, struct fat_file *file) {
  if (!file) return -1;
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0) return -1;

  struct found_entry found;
  if (fat_impl_find_entry(parent, leaf, &found) != 0) return -1;
  if (found.attr & FAT_ATTR_DIRECTORY) return -1;
  if (found.size != 0 && !cluster_is_valid(found.first_cluster)) return -1;

  fat_file_from_entry(found.raw, file);
  return 0;
}

int fat_create(const char *path, struct fat_file *file) {
  struct fat_dir parent;
  char name[FAT_LFN_MAX + 1];
  struct found_entry entry;
  if (!file || resolve_parent(path, &parent, name) ||
      fat_create_at(parent, name, &entry)) return -1;
  fat_file_from_entry(entry.raw, file);
  return 0;
}

int fat_create_at(struct fat_dir parent, const char *name, struct found_entry *out) {
  struct found_entry existing;
  if (!fat_impl_find_entry(parent, name, &existing)) return -1;
  return fat_impl_create_entry(parent, name, FAT_ATTR_ARCHIVE, 0, out);
}

void fat_file_from_entry(void *entry, struct fat_file *file) {
  *file = (struct fat_file){
    .first_cluster = fat_impl_entry_get_cluster(entry),
    .cur_cluster = fat_impl_entry_get_cluster(entry),
    .size = (u32)fat_impl_entry_get_size(entry), .dir_ent = entry
  };
}

int fat_unlink(const char *path) {
  struct fat_dir parent;
  char name[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, name)) return -1;
  return fat_remove_at(parent, name, 0);
}

int fat_remove_at(struct fat_dir parent, const char *name, int is_directory) {
  struct found_entry entry;
  if (fat_impl_find_entry(parent, name, &entry) ||
      !!(entry.attr & FAT_ATTR_DIRECTORY) != !!is_directory) return -1;
  if (is_directory && fat_impl_dir_is_empty(entry.first_cluster)) return -1;
  if (entry.first_cluster) fat_impl_free_chain(entry.first_cluster);
  fat_impl_erase_entry(parent, &entry);
  return 0;
}

int fat_mkdir(const char *path) {
  struct fat_dir parent;
  char name[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, name)) return -1;
  return fat_mkdir_at(parent, name);
}

int fat_mkdir_at(struct fat_dir parent, const char *name) {
  struct found_entry entry;
  if (!fat_impl_find_entry(parent, name, &entry)) return -1;
  u32 cluster = fat_impl_alloc_cluster();
  if (!cluster) return -1;
  if (fat_impl_init_dir_cluster(cluster, parent) ||
      fat_impl_create_entry(parent, name, FAT_ATTR_DIRECTORY, cluster, &entry)) {
    fat_impl_free_chain(cluster);
    return -1;
  }
  return 0;
}

long fat_read_dir(const char *path, u32 *index, char *buffer, usize length) {
  struct fat_dir dir;
  struct path_parts parts;
  if (split_path(path, &parts) != 0) return -1;
  if (walk_directories(&parts, parts.count, &dir) != 0) return -1;
  return fat_impl_read_dir(dir, index, buffer, length);
}
