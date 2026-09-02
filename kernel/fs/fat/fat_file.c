/* FAT byte transfers, cluster cursors and truncation. */
#include "fat_internal.h"
#include <utilities/string.h>

int fat_truncate(struct fat_file *file) {
  if (!file || !file->dir_ent) return -1;
  if (file->first_cluster)
    fat_impl_free_chain(file->first_cluster);
  file->first_cluster = 0;
  file->cur_cluster = 0;
  file->size = 0;
  file->pos = 0;
  fat_impl_entry_set_cluster(file->dir_ent, 0);
  fat_impl_entry_set_size(file->dir_ent, 0);
  fat_impl_set_timestamp(file->dir_ent);
  fat_flush_bytes(file->dir_ent, 32);
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
    u32 cluster = fat_impl_alloc_cluster();
    if (!cluster)
      return 0;
    file->first_cluster = cluster;
    file->cur_cluster = cluster;
    fat_impl_entry_set_cluster(file->dir_ent, cluster);
  }

  /* Allocate or select the next cluster for a boundary append. */
  if (length > 0 && file->pos > 0 && file->pos == file->size &&
      file->pos % bytes == 0) {
    u32 next;
    int status = next_cluster(file->cur_cluster, &next);
    if (status == 1) {
      next = fat_impl_alloc_cluster();
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
        next = fat_impl_alloc_cluster();
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
    fat_impl_entry_set_size(file->dir_ent, file->size);
  }
  fat_impl_set_timestamp(file->dir_ent);
  fat_flush_bytes(file->dir_ent, 32);

  /* cur_cluster always contains the byte at pos while pos is inside the file. */
  if (file->pos > 0 && file->pos < file->size && file->pos % bytes == 0) {
    u32 next;
    if (next_cluster(file->cur_cluster, &next) == 0)
      file->cur_cluster = next;
  }
  return total;
}
