/* Directory cursors, entry storage, metadata and VFAT slot allocation. */
#include "fat_internal.h"
#include <devices/rtc.h>
#include <utilities/string.h>

/* Cursor for walking a directory cluster chain. */
struct dir_cursor {
  struct fat_dir dir;
  u32 cluster;
  u32 slot;
  u32 index;
  u32 clusters_seen;
  int ended;
};

/* Timestamp helpers. */
static u16 fat_encode_date(const struct rtc_time *t) {
  u16 year = (u16)(t->year - 1980);
  return (u16)((year << 9) | ((u16)t->month << 5) | t->day);
}

static u16 fat_encode_time(const struct rtc_time *t) {
  return (u16)(((u16)t->hour << 11) | ((u16)t->minute << 5) |
                    (t->second / 2));
}

/* Directory-entry helpers. */

static int entry_is_lfn(const struct dir_entry *entry) {
  return (entry->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN;
}

static int entry_is_free(const struct dir_entry *entry) {
  u8 first = (u8)entry->name[0];
  return first == 0x00 || first == 0xE5;
}

static int entry_is_usable(const struct dir_entry *entry) {
  return !entry_is_free(entry) && !entry_is_lfn(entry) &&
         !(entry->attr & 0x08);
}

static int is_dot_entry(const struct dir_entry *entry) {
  return entry->name[0] == '.' &&
         (entry->name[1] == ' ' || entry->name[1] == '.');
}

/* Directory cursor. */

static int cursor_init(struct dir_cursor *cursor, struct fat_dir dir,
                       u32 start_index) {
  memset(cursor, 0, sizeof(*cursor));
  cursor->dir = dir;
  cursor->index = start_index;

  if (dir_is_fixed_root(dir)) {
    cursor->slot = start_index;
    return start_index <= fat_volume.root_entries ? 0 : -1;
  }
  if (!cluster_is_valid(dir.first_cluster))
    return -1;

  u32 entries_per_cluster = cluster_bytes() / 32;
  u32 clusters_to_skip = start_index / entries_per_cluster;
  cursor->slot = start_index % entries_per_cluster;
  cursor->cluster = dir.first_cluster;
  cursor->clusters_seen = 1;

  while (clusters_to_skip--) {
    u32 next;
    if (next_cluster(cursor->cluster, &next) != 0)
      return -1;
    cursor->cluster = next;
    if (++cursor->clusters_seen >= fat_volume.cluster_limit)
      return -1;
  }
  return 0;
}

static struct dir_entry *cursor_next(struct dir_cursor *cursor) {
  if (cursor->ended)
    return 0;

  if (dir_is_fixed_root(cursor->dir)) {
    if (cursor->slot >= fat_volume.root_entries) {
      cursor->ended = 1;
      return 0;
    }
    struct dir_entry *root = (struct dir_entry *)sector(fat_volume.root_start_sector);
    cursor->index++;
    return &root[cursor->slot++];
  }

  u32 entries_per_cluster = cluster_bytes() / 32;
  if (cursor->slot >= entries_per_cluster) {
    u32 next;
    int status = next_cluster(cursor->cluster, &next);
    if (status != 0 || ++cursor->clusters_seen >= fat_volume.cluster_limit) {
      cursor->ended = 1;
      return 0;
    }
    cursor->cluster = next;
    cursor->slot = 0;
  }

  struct dir_entry *entries = (struct dir_entry *)cluster_data(cursor->cluster);
  if (!entries) {
    cursor->ended = 1;
    return 0;
  }
  cursor->index++;
  return &entries[cursor->slot++];
}

/* Directory-slot allocation. */

static int alloc_dir_slots(struct fat_dir dir, u32 count,
                           struct dir_entry **out, u32 *start_index) {
  u32 run = 0;
  u32 run_start = 0;

  if (dir_is_fixed_root(dir)) {
    struct dir_entry *root = (struct dir_entry *)sector(fat_volume.root_start_sector);
    for (u32 i = 0; i < fat_volume.root_entries; i++) {
      if (entry_is_free(&root[i])) {
        if (run == 0)
          run_start = i;
        out[run++] = &root[i];
        if (run == count) {
          if (start_index) *start_index = run_start;
          return 0;
        }
      } else {
        run = 0;
      }
    }
    return -1;
  }

  u32 current = dir.first_cluster;
  if (!cluster_is_valid(current))
    return -1;
  u32 entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
  u32 visited = 0;

  for (;;) {
    struct dir_entry *entries = (struct dir_entry *)cluster_data(current);
    if (!entries)
      return -1;
    for (u32 i = 0; i < entries_per_cluster; i++) {
      if (entry_is_free(&entries[i])) {
        if (run == 0)
          run_start = visited * entries_per_cluster + i;
        out[run++] = &entries[i];
        if (run == count) {
          if (start_index) *start_index = run_start;
          return 0;
        }
      } else {
        run = 0;
      }
    }

    if (++visited >= fat_volume.cluster_limit)
      return -1;

    u32 next;
    int status = next_cluster(current, &next);
    if (status < 0)
      return -1;
    if (status == 1) {
      next = fat_impl_alloc_cluster();
      if (!next)
        return -1;
      fat_set(current, next);
    }
    current = next;
  }
}

int short_name_taken(struct fat_dir dir, const char name[11]) {
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, 0) != 0)
    return 0;

  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((u8)entry->name[0] == 0x00)
      return 0;
    if (!entry_is_usable(entry))
      continue;
    if (memcmp(entry->name, name, 11) == 0)
      return 1;
  }
  return 0;
}

u32 fat_impl_entry_get_cluster(void *entry) {
  struct dir_entry *e = (struct dir_entry *)entry;
  u32 low = e->first_cluster_low;
  if (fat_volume.type != FAT_TYPE_32)
    return low;
  return low | ((u32)e->first_cluster_high << 16);
}

void fat_impl_entry_set_cluster(void *entry, u32 cluster) {
  struct dir_entry *e = (struct dir_entry *)entry;
  e->first_cluster_low = (u16)(cluster & 0xFFFF);
  e->first_cluster_high = fat_volume.type == FAT_TYPE_32 ? (u16)(cluster >> 16) : 0;
}

u64 fat_impl_entry_get_size(void *entry) {
  return ((struct dir_entry *)entry)->size;
}

void fat_impl_entry_set_size(void *entry, u64 size) {
  ((struct dir_entry *)entry)->size = (u32)size;
}

void fat_impl_set_timestamp(void *entry) {
  struct dir_entry *e = (struct dir_entry *)entry;
  struct rtc_time now;
  rtc_read(&now);
  if (!now.valid)
    return;

  u16 date = fat_encode_date(&now);
  u16 time = fat_encode_time(&now);

  e->write_date = date;
  e->write_time = time;
  e->access_date = date;

  if (e->create_date == 0 && e->create_time == 0) {
    e->create_date = date;
    e->create_time = time;
    e->create_time_tenth = 0;
  }
}

int fat_impl_find_entry(struct fat_dir dir, const char *name,
                        struct found_entry *out) {
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, 0) != 0)
    return -1;

  struct lfn_state lfn;
  lfn_reset(&lfn);

  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((u8)entry->name[0] == 0x00)
      return -1;

    if (entry_is_lfn(entry)) {
      lfn_feed(&lfn, entry, cursor.index - 1);
      continue;
    }
    if (!entry_is_usable(entry)) {
      lfn_reset(&lfn);
      continue;
    }

    const char *long_name = lfn_take(&lfn, entry);
    char short_name[13];
    entry_short_name(entry, short_name);

    if ((long_name && strcasecmp(long_name, name) == 0) ||
        strcasecmp(short_name, name) == 0) {
      out->first_cluster = fat_impl_entry_get_cluster(entry);
      out->size = entry->size;
      out->attr = entry->attr;
      out->index = long_name ? lfn.start_index : cursor.index - 1;
      out->index_end = cursor.index - 1;
      out->raw = entry;
      return 0;
    }
    lfn_reset(&lfn);
  }
  return -1;
}

int fat_impl_create_entry(struct fat_dir parent, const char *name,
                          u8 attr, u32 cluster, struct found_entry *out) {
  u32 length = (u32)strlen(name);
  if (length == 0 || length > 255)
    return -1;

  char short_name[11];
  u8 nt_case = 0;
  u32 lfn_slots = 0;

  if (short_name_exact(name, length, short_name, &nt_case) != 0) {
    if (short_name_alias(parent, name, length, short_name) != 0)
      return -1;
    lfn_slots = (length + 12) / 13;
    nt_case = 0;
  }

  struct dir_entry *slots[FAT_MAX_ENTRY_SLOTS];
  u32 start_index = 0;
  if (alloc_dir_slots(parent, lfn_slots + 1, slots, &start_index) != 0)
    return -1;

  if (lfn_slots) {
    u8 checksum = lfn_checksum(short_name);
    for (u32 i = 0; i < lfn_slots; i++) {
      u8 seq = (u8)(lfn_slots - i);
      write_lfn_slot(slots[i], name, length, seq, i == 0, checksum);
    }
  }

  struct dir_entry *entry = slots[lfn_slots];
  memset(entry, 0, 32);
  memcpy(entry->name, short_name, 11);
  entry->attr = attr;
  entry->nt_case = nt_case;
  fat_impl_entry_set_cluster(entry, cluster);
  entry->size = 0;
  fat_impl_set_timestamp(entry);

  /* The run can cross between non-adjacent clusters in a directory chain. */
  for (u32 i = 0; i <= lfn_slots; i++)
    fat_flush_bytes(slots[i], sizeof(*slots[i]));

  out->first_cluster = cluster;
  out->size = 0;
  out->attr = attr;
  out->index = start_index;
  out->index_end = start_index + lfn_slots;
  out->raw = entry;
  return 0;
}

void fat_impl_erase_entry(struct fat_dir dir, struct found_entry *e) {
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, e->index) != 0)
    return;

  struct dir_entry *entry;
  while (cursor.index <= e->index_end && (entry = cursor_next(&cursor)) != 0) {
    entry->name[0] = (char)0xE5;
    fat_flush_bytes(entry, 32);
  }
}

int fat_impl_init_dir_cluster(u32 cluster, struct fat_dir parent) {
  struct dir_entry *entries = (struct dir_entry *)cluster_data(cluster);
  if (!entries)
    return -1;

  memset(entries, 0, cluster_bytes());

  memset(&entries[0], 0, sizeof(entries[0]));
  memset(entries[0].name, ' ', FAT_NAME_LEN);
  entries[0].name[0] = '.';
  entries[0].attr = FAT_ATTR_DIRECTORY;
  fat_impl_entry_set_cluster(&entries[0], cluster);
  fat_impl_set_timestamp(&entries[0]);

  memset(&entries[1], 0, sizeof(entries[1]));
  memset(entries[1].name, ' ', FAT_NAME_LEN);
  entries[1].name[0] = '.';
  entries[1].name[1] = '.';
  entries[1].attr = FAT_ATTR_DIRECTORY;
  fat_impl_entry_set_cluster(&entries[1], parent.is_root ? 0 : parent.first_cluster);
  fat_impl_set_timestamp(&entries[1]);

  fat_flush_bytes(entries, cluster_bytes());
  return 0;
}

int fat_impl_dir_is_empty(u32 cluster) {
  if (!cluster_is_valid(cluster))
    return -1;

  struct fat_dir dir = { .first_cluster = cluster, .is_root = 0 };
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, 0) != 0)
    return -1;

  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((u8)entry->name[0] == 0x00)
      return 0;
    if (!entry_is_usable(entry))
      continue;
    if (is_dot_entry(entry))
      continue;
    return -1;
  }
  return 0;
}

int fat_impl_read_dir(struct fat_dir dir, u32 *index, char *buffer, usize length) {
  if (!index || !buffer || !length) return -1;
  usize written = 0;
  for (;;) {
    u32 before = *index;
    char name[FAT_LFN_MAX + 1];
    int is_directory;
    long result = fat_impl_read_dir_one(dir, index, name, sizeof(name), &is_directory);
    if (result <= 0) return result < 0 && !written ? -1 : (int)written;
    usize count = (usize)result - 1;
    usize required = count + 1 + is_directory;
    if (required > length - written) {
      *index = before;
      return written ? (int)written : -1;
    }
    memcpy(buffer + written, name, count);
    written += count;
    if (is_directory) buffer[written++] = '/';
    buffer[written++] = 0;
  }
}

long fat_impl_read_dir_one(struct fat_dir dir, u32 *index,
                           char *buffer, usize length, int *is_dir) {
  if (!index || !buffer || !length) return -1;
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, *index) != 0)
    return -1;

  struct lfn_state lfn;
  lfn_reset(&lfn);

  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((u8)entry->name[0] == 0x00)
      break;

    if (entry_is_lfn(entry)) {
      lfn_feed(&lfn, entry, cursor.index - 1);
      continue;
    }
    if (!entry_is_usable(entry) || is_dot_entry(entry)) {
      lfn_reset(&lfn);
      continue;
    }

    const char *name = lfn_take(&lfn, entry);
    char short_name[13];
    if (!name) {
      entry_short_name(entry, short_name);
      name = short_name;
    }
    usize name_length = strlen(name);
    if (name_length + 1 > length) return -1;
    memcpy(buffer, name, name_length + 1);
    if (is_dir)
      *is_dir = (entry->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    *index = cursor.index;
    return (long)(name_length + 1);
  }

  *index = cursor.index;
  return 0;
}
