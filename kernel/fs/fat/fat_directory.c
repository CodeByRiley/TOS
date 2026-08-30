/* kernel/fs/fat/fat_directory.c , shared VFAT directory engine.
 *
 * Implements FAT16/FAT32 BPB validation, directory traversal, 8.3 aliases,
 * long filenames, allocation, timestamps, and directory mutation.
 */
#include "fat_internal.h"
#include <devices/rtc.h>
#include <utilities/string.h>
#include <utilities/log.h>

/* Private directory-entry flags. */
#define FAT_ATTR_LFN 0x0F
#define FAT_CASE_BASE_LOWER 0x08
#define FAT_CASE_EXT_LOWER 0x10
#define FAT_LFN_CHARS_PER_SLOT 13
#define FAT_LFN_MAX_SLOTS \
  ((FAT_LFN_MAX + FAT_LFN_CHARS_PER_SLOT - 1) / FAT_LFN_CHARS_PER_SLOT)
#define FAT_LFN_BUFFER (FAT_LFN_MAX_SLOTS * FAT_LFN_CHARS_PER_SLOT + 1)
#define FAT_MAX_ENTRY_SLOTS (FAT_LFN_MAX_SLOTS + 1)

/* BIOS parameter block shared by FAT16 and FAT32. */
struct PACKED bpb {
  u8 jmp[3];
  char oem[8];
  u16 bytes_per_sector;
  u8 sectors_per_cluster;
  u16 reserved_sectors;
  u8 num_fats;
  u16 root_entries;
  u16 total_sectors_16;
  u8 media;
  u16 sectors_per_fat_16;
  u16 sectors_per_track;
  u16 heads;
  u32 hidden_sectors;
  u32 total_sectors_32;
  u32 sectors_per_fat_32;
  u16 ext_flags;
  u16 fs_version;
  u32 root_cluster;
  u16 fs_info_sector;
  u16 backup_boot_sector;
};
_Static_assert(sizeof(struct bpb) == 52, "BPB layout must match on-disk");

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

/* Cursor for walking a directory cluster chain. */
struct dir_cursor {
  struct fat_dir dir;
  u32 cluster;
  u32 slot;
  u32 index;
  u32 clusters_seen;
  int ended;
};

/* Byte offsets of the 13 characters stored in one long-name slot. */
static const u8 lfn_offsets[13] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};


/* Timestamp helpers. */
static u16 fat_encode_date(const struct rtc_time *t) {
  u16 year = (u16)(t->year - 1980);
  return (u16)((year << 9) | ((u16)t->month << 5) | t->day);
}

static u16 fat_encode_time(const struct rtc_time *t) {
  return (u16)(((u16)t->hour << 11) | ((u16)t->minute << 5) |
                    (t->second / 2));
}

/* Long-filename helpers. */

static u8 lfn_checksum(const char *name) {
  u8 sum = 0;
  for (int i = 0; i < 11; i++)
    sum = (u8)(((sum & 1) << 7) + (sum >> 1) + (u8)name[i]);
  return sum;
}

static void lfn_reset(struct lfn_state *state) {
  state->length = 0;
  state->valid = 0;
  state->expect = 0;
  state->checksum = 0;
  state->start_index = 0;
}

static void lfn_feed(struct lfn_state *state, const struct dir_entry *entry,
                     u32 index) {
  const u8 *raw = (const u8 *)entry;
  u8 marker = raw[0];

  if (marker == 0xE5) {
    lfn_reset(state);
    return;
  }

  u8 seq = marker & 0x1F;
  int last = (marker & 0x40) != 0;
  if (seq == 0 || seq > FAT_LFN_MAX_SLOTS) {
    lfn_reset(state);
    return;
  }

  if (last) {
    lfn_reset(state);
    state->valid = 1;
    state->expect = seq;
    state->checksum = raw[13];
    state->length = (u32)seq * 13;
    state->start_index = index;
  } else if (!state->valid || seq != state->expect || raw[13] != state->checksum) {
    lfn_reset(state);
    return;
  }

  u32 base = (u32)(seq - 1) * 13;
  for (int i = 0; i < 13; i++) {
    u16 ch = (u16)(raw[lfn_offsets[i]] | ((u16)raw[lfn_offsets[i] + 1] << 8));
    char out;
    if (ch == 0x0000 || ch == 0xFFFF)
      out = '\0';
    else if (ch < 0x80)
      out = (char)ch;
    else
      out = '_';
    state->name[base + i] = out;
  }

  state->expect = (u8)(seq - 1);
}

static const char *lfn_take(struct lfn_state *state,
                            const struct dir_entry *entry) {
  if (!state->valid || state->expect != 0)
    return 0;
  if (lfn_checksum(entry->name) != state->checksum)
    return 0;

  u32 length = 0;
  while (length < state->length && state->name[length] != '\0')
    length++;
  if (length == 0 || length > 255)
    return 0;
  state->name[length] = '\0';
  return state->name;
}

static void write_lfn_slot(struct dir_entry *slot, const char *name,
                           u32 length, u8 seq, int last, u8 checksum) {
  u8 *raw = (u8 *)slot;
  memset(raw, 0, sizeof(*slot));
  raw[0] = (u8)(seq | (last ? 0x40 : 0));
  raw[11] = FAT_ATTR_LFN;
  raw[13] = checksum;

  u32 base = (u32)(seq - 1) * 13;
  for (int i = 0; i < 13; i++) {
    u32 idx = base + (u32)i;
    u16 ch;
    if (idx < length)
      ch = (u16)(u8)name[idx];
    else if (idx == length)
      ch = 0x0000;
    else
      ch = 0xFFFF;
    raw[lfn_offsets[i]] = (u8)(ch & 0xFF);
    raw[lfn_offsets[i] + 1] = (u8)(ch >> 8);
  }
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
    return start_index <= root_entries ? 0 : -1;
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
    if (++cursor->clusters_seen >= cluster_limit)
      return -1;
  }
  return 0;
}

static struct dir_entry *cursor_next(struct dir_cursor *cursor) {
  if (cursor->ended)
    return 0;

  if (dir_is_fixed_root(cursor->dir)) {
    if (cursor->slot >= root_entries) {
      cursor->ended = 1;
      return 0;
    }
    struct dir_entry *root = (struct dir_entry *)sector(root_start_sec);
    cursor->index++;
    return &root[cursor->slot++];
  }

  u32 entries_per_cluster = cluster_bytes() / 32;
  if (cursor->slot >= entries_per_cluster) {
    u32 next;
    int status = next_cluster(cursor->cluster, &next);
    if (status != 0 || ++cursor->clusters_seen >= cluster_limit) {
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
    struct dir_entry *root = (struct dir_entry *)sector(root_start_sec);
    for (u32 i = 0; i < root_entries; i++) {
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

    if (++visited >= cluster_limit)
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

/* Short-name helpers. */
static int valid_short_char(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9'))
    return 1;
  switch (c) {
  case '$': case '%': case '\'': case '-': case '_':
  case '@': case '~': case '`': case '!': case '(':
  case ')': case '{': case '}': case '^': case '#': case '&':
    return 1;
  default:
    return 0;
  }
}

static char to_upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }

static void split_extension(const char *name, u32 length,
                            u32 *base_len, u32 *ext_at, u32 *ext_len) {
  u32 dot = length;
  for (u32 i = length; i > 1; i--) {
    if (name[i - 1] == '.') {
      dot = i - 1;
      break;
    }
  }
  *base_len = dot;
  *ext_at = dot < length ? dot + 1 : length;
  *ext_len = length - *ext_at;
}

static int short_name_exact(const char *name, u32 length,
                            char out[11], u8 *nt_case) {
  if (length == 0 || length > 12 || name[length - 1] == '.')
    return -1;

  u32 base_len, ext_at, ext_len;
  split_extension(name, length, &base_len, &ext_at, &ext_len);
  if (base_len == 0 || base_len > 8 || ext_len > 3)
    return -1;

  int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
  for (u32 i = 0; i < base_len; i++) {
    if (!valid_short_char(name[i]))
      return -1;
    if (is_lower(name[i])) base_lower = 1;
    if (is_upper(name[i])) base_upper = 1;
  }
  for (u32 i = 0; i < ext_len; i++) {
    char c = name[ext_at + i];
    if (!valid_short_char(c))
      return -1;
    if (is_lower(c)) ext_lower = 1;
    if (is_upper(c)) ext_upper = 1;
  }
  if ((base_lower && base_upper) || (ext_lower && ext_upper))
    return -1;

  memset(out, ' ', 11);
  for (u32 i = 0; i < base_len; i++)
    out[i] = to_upper(name[i]);
  for (u32 i = 0; i < ext_len; i++)
    out[8 + i] = to_upper(name[ext_at + i]);

  if ((u8)out[0] == 0xE5)
    out[0] = 0x05;

  *nt_case = (u8)((base_lower ? FAT_CASE_BASE_LOWER : 0) |
                       (ext_lower ? FAT_CASE_EXT_LOWER : 0));
  return 0;
}

static int short_name_taken(struct fat_dir dir, const char name[11]) {
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

static int short_name_alias(struct fat_dir dir, const char *name,
                            u32 length, char out[11]) {
  u32 base_len, ext_at, ext_len;
  split_extension(name, length, &base_len, &ext_at, &ext_len);

  char base[8];
  u32 base_used = 0;
  for (u32 i = 0; i < base_len && base_used < 8; i++) {
    char c = name[i];
    if (c == ' ' || c == '.')
      continue;
    base[base_used++] = valid_short_char(c) ? to_upper(c) : '_';
  }
  if (base_used == 0)
    base[base_used++] = '_';

  char ext[3];
  u32 ext_used = 0;
  for (u32 i = 0; i < ext_len && ext_used < 3; i++) {
    char c = name[ext_at + i];
    if (c == ' ' || c == '.')
      continue;
    ext[ext_used++] = valid_short_char(c) ? to_upper(c) : '_';
  }

  for (u32 n = 1; n <= 999999; n++) {
    char suffix[7];
    u32 suffix_len = 0;
    for (u32 value = n; value; value /= 10)
      suffix[suffix_len++] = (char)('0' + value % 10);

    u32 stem = 8 - (suffix_len + 1);
    if (stem > base_used)
      stem = base_used;

    memset(out, ' ', 11);
    for (u32 i = 0; i < stem; i++)
      out[i] = base[i];
    out[stem] = '~';
    for (u32 i = 0; i < suffix_len; i++)
      out[stem + 1 + i] = suffix[suffix_len - 1 - i];
    for (u32 i = 0; i < ext_used; i++)
      out[8 + i] = ext[i];

    if (!short_name_taken(dir, out))
      return 0;
  }
  return -1;
}

static u32 entry_short_name(const struct dir_entry *entry, char *out) {
  u32 length = 0;
  int base_lower = (entry->nt_case & FAT_CASE_BASE_LOWER) != 0;
  int ext_lower = (entry->nt_case & FAT_CASE_EXT_LOWER) != 0;

  for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
    char c = entry->name[i];
    if (i == 0 && (u8)c == 0x05)
      c = (char)0xE5;
    out[length++] = base_lower && is_upper(c) ? (char)(c + ('a' - 'A')) : c;
  }

  int has_extension = 0;
  for (int i = 0; i < 3; i++) {
    if (entry->ext[i] != ' ') {
      has_extension = 1;
      break;
    }
  }
  if (has_extension) {
    out[length++] = '.';
    for (int i = 0; i < 3 && entry->ext[i] != ' '; i++) {
      char c = entry->ext[i];
      out[length++] = ext_lower && is_upper(c) ? (char)(c + ('a' - 'A')) : c;
    }
  }
  out[length] = '\0';
  return length;
}

u32 fat_impl_entry_get_cluster(void *entry) {
  struct dir_entry *e = (struct dir_entry *)entry;
  u32 low = e->first_cluster_low;
  if (fat_type != FAT_TYPE_32)
    return low;
  return low | ((u32)e->first_cluster_high << 16);
}

void fat_impl_entry_set_cluster(void *entry, u32 cluster) {
  struct dir_entry *e = (struct dir_entry *)entry;
  e->first_cluster_low = (u16)(cluster & 0xFFFF);
  e->first_cluster_high = fat_type == FAT_TYPE_32 ? (u16)(cluster >> 16) : 0;
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

int fat_mount_format(u8 *image, usize size, enum fat_type expected_type) {
  if (!image || size < 512)
    return -1;

  struct bpb *b = (struct bpb *)image;
  u32 total_sectors =
      b->total_sectors_16 ? b->total_sectors_16 : b->total_sectors_32;
  u32 fat_sectors =
      b->sectors_per_fat_16 ? b->sectors_per_fat_16 : b->sectors_per_fat_32;

  if (b->bytes_per_sector < 512 || b->bytes_per_sector > 4096 ||
      (b->bytes_per_sector & (b->bytes_per_sector - 1)) != 0 ||
      b->sectors_per_cluster == 0 || b->reserved_sectors == 0 ||
      b->num_fats == 0 || fat_sectors == 0 || total_sectors == 0 ||
      (u64)total_sectors * b->bytes_per_sector > size)
    return -1;

  u32 root_sectors =
      ((u32)b->root_entries * 32 + b->bytes_per_sector - 1) /
      b->bytes_per_sector;
  u32 fat_start = b->reserved_sectors;
  u32 root_start = fat_start + (u32)b->num_fats * fat_sectors;
  u32 data_start = root_start + root_sectors;
  if (root_start < fat_start || data_start < root_start ||
      data_start >= total_sectors)
    return -1;

  u32 data_clusters =
      (total_sectors - data_start) / b->sectors_per_cluster;

  enum fat_type type;
  if (data_clusters < 4085)
    return -1;
  else if (data_clusters < 65525)
    type = FAT_TYPE_16;
  else
    type = FAT_TYPE_32;

  if (type != expected_type)
    return -1;

  u32 entry_bytes = type == FAT_TYPE_32 ? 4 : 2;
  u32 fat_entries = (u32)((u64)fat_sectors * b->bytes_per_sector / entry_bytes);
  u32 limit = data_clusters + 2;
  if (limit > fat_entries)
    limit = fat_entries;
  if (limit <= 2)
    return -1;
  if (type == FAT_TYPE_16 && limit >= 0xFFF0)
    return -1;

  if (type == FAT_TYPE_16) {
    if (b->root_entries == 0 || root_sectors == 0)
      return -1;
  } else {
    if (b->root_entries != 0 || root_sectors != 0)
      return -1;
    if (b->root_cluster < 2 || b->root_cluster >= limit)
      return -1;
  }

  fs_image = image;
  fs_image_size = size;
  bytes_per_sec = b->bytes_per_sector;
  sec_per_clus = b->sectors_per_cluster;
  num_fats = b->num_fats;
  sectors_per_fat = fat_sectors;
  fat_start_sec = fat_start;
  root_start_sec = root_start;
  data_start_sec = data_start;
  root_entries = b->root_entries;
  fat_type = type;
  cluster_limit = limit;
  root_cluster = type == FAT_TYPE_32 ? b->root_cluster : 0;

  log_write_hex("FAT: type          =", (u64)fat_type, KERNEL, LOG_INFO);
  log_write_hex("FAT: bytes/sector  =", bytes_per_sec, KERNEL, LOG_INFO);
  log_write_hex("FAT: sec/cluster   =", sec_per_clus, KERNEL, LOG_INFO);
  log_write_hex("FAT: clusters      =", cluster_limit, KERNEL, LOG_INFO);
  return 0;
}

u32 fat_impl_alloc_cluster(void) {
  for (u32 c = 2; c < cluster_limit; c++) {
    if (fat_get(c) == 0) {
      fat_set(c, cluster_eoc());
      memset(cluster_data(c), 0, cluster_bytes());
      /* Freed clusters can contain old directory entries. Persist the clear
       * before publishing new contents so they cannot return after reboot. */
      fat_flush_bytes(cluster_data(c), cluster_bytes());
      return c;
    }
  }
  return 0;
}

void fat_impl_free_chain(u32 first) {
  u32 current = first;
  u32 visited = 0;
  while (cluster_is_valid(current) && visited++ < cluster_limit) {
    u32 next = fat_get(current);
    fat_set(current, 0);
    if (cluster_is_eoc(next) || !cluster_is_valid(next))
      break;
    current = next;
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

int fat_impl_read_dir(struct fat_dir dir, u32 *index,
                      char *buffer, usize length) {
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, *index) != 0)
    return -1;

  struct lfn_state lfn;
  lfn_reset(&lfn);

  usize written = 0;
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

    const char *long_name = lfn_take(&lfn, entry);
    char name[260];
    usize name_length;
    if (long_name) {
      name_length = strlen(long_name);
      memcpy(name, long_name, name_length);
    } else {
      name_length = entry_short_name(entry, name);
    }
    if (entry->attr & FAT_ATTR_DIRECTORY)
      name[name_length++] = '/';
    name[name_length] = '\0';

    usize required = name_length + 1;
    if (required > length - written) {
      if (written == 0)
        return -1;
      *index = long_name ? lfn.start_index : cursor.index - 1;
      break;
    }
    memcpy(buffer + written, name, required);
    written += required;
    lfn_reset(&lfn);
  }
  *index = cursor.index;
  return (int)written;
}

long fat_impl_read_dir_one(struct fat_dir dir, u32 *index,
                           char *buffer, usize length, int *is_dir) {
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

    const char *long_name = lfn_take(&lfn, entry);
    usize name_length;
    if (long_name) {
      name_length = strlen(long_name);
      if (name_length + 1 > length)
        return -1;
      memcpy(buffer, long_name, name_length);
    } else {
      name_length = entry_short_name(entry, buffer);
      if (name_length + 1 > length)
        return -1;
    }
    buffer[name_length] = 0;
    if (is_dir)
      *is_dir = (entry->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    *index = cursor.index;
    return (long)(name_length + 1);
  }

  *index = cursor.index;
  return 0;
}
