/* Minimal read/write FAT16 + FAT32 driver over an in-memory disk image.
 *
 * The flavour is detected from the cluster count at mount time, so the two
 * differ here in exactly three places: the width of a FAT entry, where the
 * root directory lives (a fixed table on FAT16, a normal cluster chain on
 * FAT32), and where the high half of an entry's first cluster is stored.
 * Everything above that is shared.
 *
 * Paths are root-relative and case-insensitive. Both '/' and '\\' separate
 * components. VFAT long names are read and written: a created entry gets
 * LFN slots plus a unique 8.3 alias, and a name that already fits 8.3 is
 * written as a plain short entry with the NT case flags so it round-trips
 * without costing a slot.
 */
#include <devices/rtc.h>
#include <fs/fat/fat.h>
#include <utilities/log.h>
#include <utilities/string.h>

/* Directory entry attribute byte (offset 11) */
//
// Bits | Name       | Description
// 7-6  | Reserved   |
// 5    | Archive    | Set on write; backup software clears it
// 4    | Directory  | Entry's cluster chain holds directory entries
// 3    | Volume ID  | Entry is the volume label, not a file
// 2    | System     |
// 1    | Hidden     |
// 0    | Read Only  |
//
// FAT_ATTR_LFN is not a bit but the value 0x0F , all four of the low bits at
// once. That combination is meaningless for a real file, which is exactly why
// it was chosen: an old 8.3-only driver sees read-only+hidden+system+volume-id
// and skips the entry, so long-name slots stay invisible to it.
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYSTEM 0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE 0x20
#define FAT_ATTR_LFN 0x0F

/* NT case flags in the short entry's reserved byte. Set when the base or
 * extension is stored uppercase but should be displayed lowercase. */
#define FAT_CASE_BASE_LOWER 0x08
#define FAT_CASE_EXT_LOWER 0x10

#define FAT_MAX_COMPONENTS 16
#define FAT_LFN_CHARS_PER_SLOT 13
#define FAT_LFN_MAX_SLOTS ((FAT_LFN_MAX + FAT_LFN_CHARS_PER_SLOT - 1) / FAT_LFN_CHARS_PER_SLOT)
/* One short entry plus every LFN slot that can precede it. */
#define FAT_MAX_SLOTS (FAT_LFN_MAX_SLOTS + 1)
/* A full complement of slots carries 260 characters, more than the 255 a
 * name may actually use. The accumulator has to hold the slots, not the
 * name, or a malformed run walks off the end of it. */
#define FAT_LFN_BUFFER (FAT_LFN_MAX_SLOTS * FAT_LFN_CHARS_PER_SLOT + 1)

enum fat_type {
  FAT_TYPE_NONE = 0,
  FAT_TYPE_16 = 16,
  FAT_TYPE_32 = 32,
};

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
  /* FAT32 extended block. Only meaningful when sectors_per_fat_16 is 0. */
  u32 sectors_per_fat_32;
  u16 ext_flags;
  u16 fs_version;
  u32 root_cluster;
  u16 fs_info_sector;
  u16 backup_boot_sector;
};

_Static_assert(sizeof(struct bpb) == 52, "BPB layout must match on-disk");

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

struct fat_dir {
  u32 first_cluster;
  int is_root;
};

struct path_parts {
  const char *component[FAT_MAX_COMPONENTS];
  u8 length[FAT_MAX_COMPONENTS];
  u8 count;
};

struct dir_cursor {
  struct fat_dir dir;
  u32 cluster;
  u32 slot;
  u32 index;
  u32 clusters_seen;
  int ended;
};

/* A resolved directory entry plus the slot range it occupies. `index` is
 * the cursor index of the short entry; `lfn_index` is where its LFN run
 * starts (equal to `index` when the entry has no long name). Deleting an
 * entry has to erase the whole range, not just the short slot. */
struct dir_slot {
  struct dir_entry *entry;
  u32 index;
  u32 lfn_index;
};

/* Long-name accumulator. LFN slots precede their short entry and are
 * stored last-fragment-first, so a run is only trustworthy once the short
 * entry arrives and its checksum matches. */
struct lfn_state {
  char name[FAT_LFN_BUFFER];
  u32 length;
  u32 start_index;
  u8 checksum;
  u8 expect;
  u8 valid;
};

static u8 *fs_image;
static usize fs_image_size;
static u16 bytes_per_sec;
static u8 sec_per_clus;
static u8 num_fats;
static u32 sectors_per_fat;
static u32 fat_start_sec;
static u32 root_start_sec;
static u32 data_start_sec;
static u16 root_entries;
static u32 root_cluster;
static u32 fsinfo_sec;
static enum fat_type fat_type;
/* Exclusive upper bound; valid data clusters are [2, cluster_limit). */
static u32 cluster_limit;

/* Installed by the storage backend once an image is mounted from a real
 * device. NULL means the RAM array is the only copy that exists, which is
 * both the pre-mount state and how the host tests run. */
static fat_sector_writer sector_writer;

/* Byte offsets of the 13 name characters inside an LFN slot. */
static const u8 lfn_offsets[FAT_LFN_CHARS_PER_SLOT] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static u32 cluster_bytes(void) {
  return (u32)sec_per_clus * bytes_per_sec;
}

static u8 *sector(u32 lba) {
  return fs_image + (u64)lba * bytes_per_sec;
}

static int cluster_is_valid(u32 cluster) {
  return cluster >= 2 && cluster < cluster_limit;
}

static u32 cluster_eoc(void) {
  return fat_type == FAT_TYPE_32 ? 0x0FFFFFFFu : 0xFFFFu;
}

static int cluster_is_eoc(u32 value) {
  return fat_type == FAT_TYPE_32 ? value >= 0x0FFFFFF8u : value >= 0xFFF8u;
}

static int cluster_is_bad(u32 value) {
  return fat_type == FAT_TYPE_32 ? value == 0x0FFFFFF7u : value == 0xFFF7u;
}

/* FAT16 entries are 16 bits, FAT32 entries 28 bits inside a 32-bit slot. */
static u32 fat_get(u32 cluster) {
  u8 *table = sector(fat_start_sec);
  if (fat_type == FAT_TYPE_32)
    return ((u32 *)table)[cluster] & 0x0FFFFFFFu;
  return ((u16 *)table)[cluster];
}

/* The FAT32 entry's top 4 bits are reserved and must survive a write. */
static void fat_put(u8 *table, u32 cluster, u32 value) {
  if (fat_type == FAT_TYPE_32) {
    u32 *slots = (u32 *)table;
    slots[cluster] = (slots[cluster] & 0xF0000000u) | (value & 0x0FFFFFFFu);
  } else {
    ((u16 *)table)[cluster] = (u16)value;
  }
}

/* The free-cluster counters cached in FSInfo go stale the moment we
 * allocate. Rather than track them, mark them unknown so a host that
 * mounts this image recomputes instead of trusting our arithmetic. */
static void fsinfo_invalidate(void) {
  if (!fsinfo_sec)
    return;
  u8 *info = sector(fsinfo_sec);
  if (*(u32 *)info != 0x41615252u)
    return;
  *(u32 *)(info + 488) = 0xFFFFFFFFu; /* free cluster count  */
  *(u32 *)(info + 492) = 0xFFFFFFFFu; /* next-free-cluster hint */
}

/* Return 0 for a valid successor, 1 for EOC, and -1 for corruption. */
static int next_cluster(u32 cluster, u32 *next) {
  if (!cluster_is_valid(cluster))
    return -1;
  u32 value = fat_get(cluster);
  if (cluster_is_eoc(value))
    return 1;
  if (cluster_is_bad(value) || !cluster_is_valid(value))
    return -1;
  *next = value;
  return 0;
}

static u8 *cluster_data(u32 cluster) {
  if (!cluster_is_valid(cluster))
    return 0;
  u32 lba = data_start_sec + (cluster - 2) * sec_per_clus;
  return sector(lba);
}

/* FAT16 keeps first_cluster_high reserved and zero; only FAT32 uses it. */
static u32 entry_cluster(const struct dir_entry *entry) {
  u32 low = entry->first_cluster_low;
  if (fat_type != FAT_TYPE_32)
    return low;
  return low | ((u32)entry->first_cluster_high << 16);
}

static void entry_set_cluster(struct dir_entry *entry, u32 cluster) {
  entry->first_cluster_low = (u16)(cluster & 0xFFFF);
  entry->first_cluster_high =
      fat_type == FAT_TYPE_32 ? (u16)(cluster >> 16) : 0;
}

int fat_type_bits(void) { return (int)fat_type; }

int fat_init(u8 *image, usize size) {
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
      ((u32)b->root_entries * sizeof(struct dir_entry) +
       b->bytes_per_sector - 1) /
      b->bytes_per_sector;
  u32 fat_start = b->reserved_sectors;
  u32 root_start = fat_start + (u32)b->num_fats * fat_sectors;
  u32 data_start = root_start + root_sectors;
  if (root_start < fat_start || data_start < root_start ||
      data_start >= total_sectors)
    return -1;

  u32 data_clusters =
      (total_sectors - data_start) / b->sectors_per_cluster;

  /* The cluster count is the only thing that decides the FAT flavour --
   * not the BPB's OEM string, and not the presence of the FAT32 block. */
  enum fat_type type;
  if (data_clusters < 4085)
    return -1; /* FAT12 is not supported */
  else if (data_clusters < 65525)
    type = FAT_TYPE_16;
  else
    type = FAT_TYPE_32;

  u32 entry_bytes = type == FAT_TYPE_32 ? 4 : 2;
  u32 fat_entries =
      (u32)((u64)fat_sectors * b->bytes_per_sector / entry_bytes);
  u32 limit = data_clusters + 2;
  if (limit > fat_entries)
    limit = fat_entries;
  if (limit <= 2)
    return -1;
  if (type == FAT_TYPE_16 && limit >= 0xFFF0)
    return -1;

  /* FAT16 keeps a fixed root table; FAT32 stores the root as a chain and
   * must therefore name a real starting cluster. */
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
  fsinfo_sec = 0;
  if (type == FAT_TYPE_32 && b->fs_info_sector &&
      b->fs_info_sector != 0xFFFF && b->fs_info_sector < fat_start)
    fsinfo_sec = b->fs_info_sector;

  (void)fs_image_size;
  log_write_hex("FAT: type          =", (u64)fat_type, KERNEL, LOG_INFO);
  log_write_hex("FAT: bytes/sector  =", bytes_per_sec, KERNEL, LOG_INFO);
  log_write_hex("FAT: sec/cluster   =", sec_per_clus, KERNEL, LOG_INFO);
  log_write_hex("FAT: fat start sec =", fat_start_sec, KERNEL, LOG_INFO);
  log_write_hex("FAT: root start    =", root_start_sec, KERNEL, LOG_INFO);
  log_write_hex("FAT: root cluster  =", root_cluster, KERNEL, LOG_INFO);
  log_write_hex("FAT: data start    =", data_start_sec, KERNEL, LOG_INFO);
  log_write_hex("FAT: clusters      =", cluster_limit, KERNEL, LOG_INFO);
  return 0;
}

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
    while (pos < FAT_PATH_MAX && path[pos] != '\0' && !is_separator(path[pos]))
      pos++;
    if (pos >= FAT_PATH_MAX)
      return -1;

    u32 length = pos - start;
    if (length == 1 && path[start] == '.')
      continue;
    if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
      if (parts->count > 0)
        parts->count--;
      continue;
    }
    if (length == 0 || length > FAT_LFN_MAX ||
        parts->count >= FAT_MAX_COMPONENTS)
      return -1;

    parts->component[parts->count] = path + start;
    parts->length[parts->count] = (u8)length;
    parts->count++;
  }
}

/* Characters legal in a raw 8.3 field. Space is excluded on purpose: it is
 * the padding byte, so a name containing one cannot be stored short. */
static int valid_short_char(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9'))
    return 1;
  switch (c) {
  case '$':
  case '%':
  case '\'':
  case '-':
  case '_':
  case '@':
  case '~':
  case '`':
  case '!':
  case '(':
  case ')':
  case '{':
  case '}':
  case '^':
  case '#':
  case '&':
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

/* Split a name at its final '.'. A leading dot is part of the base, so
 * ".config" has no extension rather than an empty one. */
static void split_extension(const char *name, u32 length,
                            u32 *base_len, u32 *ext_at,
                            u32 *ext_len) {
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

/* Try to store `name` as a plain 8.3 entry. Succeeds only when every
 * character is legal, both halves fit, and each half has uniform case --
 * anything else needs LFN slots to survive a round trip. `nt_case` comes
 * back with the flags that restore the original case on read. */
static int short_name_exact(const char *name, u32 length,
                            char out[FAT_NAME_LEN], u8 *nt_case) {
  /* A trailing dot has no 8.3 spelling: the extension field would be
   * empty and the name would read back without the dot. */
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
    if (is_lower(name[i]))
      base_lower = 1;
    if (is_upper(name[i]))
      base_upper = 1;
  }
  for (u32 i = 0; i < ext_len; i++) {
    char c = name[ext_at + i];
    if (!valid_short_char(c))
      return -1;
    if (is_lower(c))
      ext_lower = 1;
    if (is_upper(c))
      ext_upper = 1;
  }
  if ((base_lower && base_upper) || (ext_lower && ext_upper))
    return -1;

  memset(out, ' ', FAT_NAME_LEN);
  for (u32 i = 0; i < base_len; i++)
    out[i] = to_upper(name[i]);
  for (u32 i = 0; i < ext_len; i++)
    out[8 + i] = to_upper(name[ext_at + i]);

  /* 0xE5 is the deleted marker; the standard escape is 0x05. */
  if ((u8)out[0] == 0xE5)
    out[0] = 0x05;

  *nt_case = (u8)((base_lower ? FAT_CASE_BASE_LOWER : 0) |
                       (ext_lower ? FAT_CASE_EXT_LOWER : 0));
  return 0;
}

/* Checksum tying an LFN run to its short entry. Any edit to the short name
 * by an 8.3-only writer breaks it, which is exactly how such a writer
 * signals that the long name is now stale. */
static u8 lfn_checksum(const char *name) {
  u8 sum = 0;
  for (int i = 0; i < FAT_NAME_LEN; i++)
    sum = (u8)(((sum & 1) << 7) + (sum >> 1) + (u8)name[i]);
  return sum;
}

static struct fat_dir root_directory(void) {
  struct fat_dir dir;
  dir.is_root = 1;
  dir.first_cluster = fat_type == FAT_TYPE_32 ? root_cluster : 0;
  return dir;
}

/* Only FAT16 has a root outside the cluster space. On FAT32 the root is an
 * ordinary chain, so every chain-walking path below handles it unchanged. */
static int dir_is_fixed_root(struct fat_dir dir) {
  return dir.is_root && fat_type != FAT_TYPE_32;
}

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

  u32 entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
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

  u32 entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
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

/* Push one sector of the RAM array at whatever is backing it. A no-op when
 * no backend is installed , the image is then RAM-only and already current. */
static void fat_flush_sector(u32 lba) {
    if (!fs_image || !sector_writer) return;

    sector_writer(lba, sector(lba));
}

/* Push every sector overlapping [start, start + len) back to the backing
 * store. Callers pass a pointer into fs_image and a byte count.
 *
 * Deriving the sector span here rather than at each call site is deliberate:
 * fat_set used to compute its own LBA from the start of the FAT copy instead
 * of the sector holding the modified entry, so on any volume whose FAT spans
 * more than one sector it wrote the wrong sector to disk. One implementation
 * of this arithmetic is one place for that to be wrong. */
static void fat_flush_bytes(const void *start, usize len) {
  if (!fs_image || !sector_writer || !start || len == 0 || !bytes_per_sec)
    return;

  u64 base = (u64)(uintptr_t)fs_image;
  u64 at = (u64)(uintptr_t)start;
  if (at < base)
    return;

  u64 off = at - base;
  if (off >= fs_image_size)
    return;
  if (len > fs_image_size - off)
    len = (usize)(fs_image_size - off);

  u32 first = (u32)(off / bytes_per_sec);
  u32 last = (u32)((off + len - 1) / bytes_per_sec);
  for (u32 lba = first; lba <= last; lba++)
    fat_flush_sector(lba);
}

static void erase_slots(struct fat_dir dir, u32 from, u32 to) {
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, from) != 0)
    return;

  struct dir_entry *entry;
  while (cursor.index <= to && (entry = cursor_next(&cursor)) != 0) {
    entry->name[0] = (char)0xE5; // Mark as deleted
    fat_flush_bytes(entry, sizeof(*entry));
  }
}

static int entry_is_lfn(const struct dir_entry *entry) {
  return (entry->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN;
}

/* Mirror writes to every FAT copy. */
static void fat_set(u32 cluster, u32 value) {
  /* Width of one table entry, which is also the stride fat_put indexes by. */
  usize entry_width = (fat_type == FAT_TYPE_32) ? 4u : 2u;

  for (u8 f = 0; f < num_fats; f++) {
    u8 *table = sector(fat_start_sec + f * sectors_per_fat);
    fat_put(table, cluster, value);

    /* Flush the sector holding this cluster's entry, not the first sector of
     * the table , those are the same thing only for cluster numbers inside
     * the first sector. */
    fat_flush_bytes(table + (usize)cluster * entry_width, entry_width);
  }
  fsinfo_invalidate();
  // Also flush the FSInfo sector if it exists!
  if (fsinfo_sec) fat_flush_sector(fsinfo_sec);
}

static int entry_is_free(const struct dir_entry *entry) {
  u8 first = (u8)entry->name[0];
  return first == 0x00 || first == 0xE5;
}

static int entry_is_usable(const struct dir_entry *entry) {
  return !entry_is_free(entry) && !entry_is_lfn(entry) &&
         !(entry->attr & FAT_ATTR_VOLUME_ID);
}

static void lfn_reset(struct lfn_state *state) {
  state->length = 0;
  state->valid = 0;
  state->expect = 0;
  state->checksum = 0;
  state->start_index = 0;
}

/* Feed one LFN slot. Slots arrive highest-sequence-first, so a run opens on
 * the slot carrying the 0x40 "last" bit and must then count down to 1 with
 * a constant checksum. Anything out of order drops the run rather than
 * splicing together fragments of two different names. */
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
    state->length = (u32)seq * FAT_LFN_CHARS_PER_SLOT;
    state->start_index = index;
  } else if (!state->valid || seq != state->expect || raw[13] != state->checksum) {
    lfn_reset(state);
    return;
  }

  u32 base = (u32)(seq - 1) * FAT_LFN_CHARS_PER_SLOT;
  for (int i = 0; i < FAT_LFN_CHARS_PER_SLOT; i++) {
    u16 ch =
        (u16)(raw[lfn_offsets[i]] | ((u16)raw[lfn_offsets[i] + 1] << 8));
    /* 0xFFFF is padding past the terminator. Anything outside 7-bit ASCII
     * has no representation here, so it becomes '_' and simply will not
     * match a query -- which beats truncating the name silently. */
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

/* Close a run against its short entry. Returns the long name, or NULL when
 * there is no complete, checksum-matching run in front of this entry. */
static const char *lfn_take(struct lfn_state *state,
                            const struct dir_entry *entry) {
  if (!state->valid || state->expect != 0)
    return 0;
  if (lfn_checksum(entry->name) != state->checksum)
    return 0;

  /* The stored length is a whole number of slots; the real name ends at
   * the terminator when the final slot is not full. A run with no
   * terminator inside 260 characters claims a name longer than the format
   * allows, so treat it as corrupt rather than truncating it. */
  u32 length = 0;
  while (length < state->length && state->name[length] != '\0')
    length++;
  if (length == 0 || length > FAT_LFN_MAX)
    return 0;
  state->name[length] = '\0';
  return state->name;
}

/* Render an entry's 8.3 name for display, honouring the NT case flags. */
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

static int name_matches(const char *candidate, const char *target) {
  return candidate && strcasecmp(candidate, target) == 0;
}

/* Look up one component. A file is findable by either of its names: the
 * long one, and the 8.3 alias that always exists alongside it. */
static int find_in_directory(struct fat_dir dir, const char *target,
                             struct dir_slot *out) {
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
    char short_name[FAT_NAME_LEN + 2];
    entry_short_name(entry, short_name);

    if (name_matches(long_name, target) || name_matches(short_name, target)) {
      out->entry = entry;
      out->index = cursor.index - 1;
      out->lfn_index = long_name ? lfn.start_index : cursor.index - 1;
      return 0;
    }
    lfn_reset(&lfn);
  }
  return -1;
}

static int walk_directories(const struct path_parts *parts, u32 count,
                            struct fat_dir *out) {
  struct fat_dir current = root_directory();
  for (u32 i = 0; i < count; i++) {
    char component[FAT_LFN_MAX + 1];
    memcpy(component, parts->component[i], parts->length[i]);
    component[parts->length[i]] = '\0';

    struct dir_slot slot;
    if (find_in_directory(current, component, &slot) != 0)
      return -1;
    if (!(slot.entry->attr & FAT_ATTR_DIRECTORY))
      return -1;

    u32 cluster = entry_cluster(slot.entry);
    /* ".." pointing at the root is stored as cluster 0 by convention. */
    if (cluster == 0) {
      current = root_directory();
      continue;
    }
    if (!cluster_is_valid(cluster))
      return -1;
    current.is_root = 0;
    current.first_cluster = cluster;
  }
  *out = current;
  return 0;
}

static int resolve_parent(const char *path, struct fat_dir *parent,
                          char leaf[FAT_LFN_MAX + 1]) {
  struct path_parts parts;
  if (split_path(path, &parts) != 0 || parts.count == 0)
    return -1;

  u32 last = (u32)parts.count - 1;
  memcpy(leaf, parts.component[last], parts.length[last]);
  leaf[parts.length[last]] = '\0';

  return walk_directories(&parts, last, parent);
}

static int resolve_slot(const char *path, struct fat_dir *parent_out,
                        struct dir_slot *out) {
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  if (resolve_parent(path, &parent, leaf) != 0)
    return -1;
  if (parent_out)
    *parent_out = parent;
  return find_in_directory(parent, leaf, out);
}

static struct dir_entry *resolve_entry(const char *path,
                                       struct fat_dir *parent_out) {
  struct dir_slot slot;
  if (resolve_slot(path, parent_out, &slot) != 0)
    return 0;
  return slot.entry;
}

static int resolve_directory(const char *path, struct fat_dir *out) {
  struct path_parts parts;
  if (split_path(path, &parts) != 0)
    return -1;
  return walk_directories(&parts, parts.count, out);
}

int fat_open(const char *path, struct fat_file *file) {
  if (!file)
    return -1;
  struct dir_entry *entry = resolve_entry(path, 0);
  if (!entry || (entry->attr & FAT_ATTR_DIRECTORY))
    return -1;

  u32 cluster = entry_cluster(entry);
  if (entry->size != 0 && !cluster_is_valid(cluster))
    return -1;

  file->first_cluster = cluster;
  file->cur_cluster = cluster;
  file->size = entry->size;
  file->pos = 0;
  file->dir_ent = entry;
  return 0;
}

int fat_stat(const char *path, struct fat_stat *out) {
  if (!out)
    return -1;

  /* The root has no directory entry of its own, so resolve_entry can't
   * see it. Anything that resolves as a directory path with no leaf is
   * the root: report it as an empty directory. */
  struct fat_dir dir;
  if (resolve_directory(path, &dir) == 0 && dir.is_root) {
    out->size = 0;
    out->first_cluster = root_cluster;
    out->attr = FAT_ATTR_DIRECTORY;
    out->is_dir = 1;
    return 0;
  }

  struct dir_entry *entry = resolve_entry(path, 0);
  if (!entry)
    return -1;

  out->attr = entry->attr;
  out->is_dir = (entry->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
  out->first_cluster = entry_cluster(entry);
  out->size = out->is_dir ? 0 : entry->size;
  return 0;
}

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

    if (file->pos % bytes == 0 && file->pos < file->size) {
      u32 next;
      if (next_cluster(file->cur_cluster, &next) != 0)
        break;
      file->cur_cluster = next;
    }
  }
  return total;
}

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
  /* At an exact EOF boundary there is no containing cluster. Keep the
   * cursor on the final cluster so a subsequent append can extend it. */
  if (position == file->size && position % bytes == 0)
    skip--;
  while (skip--) {
    u32 next;
    if (next_cluster(file->cur_cluster, &next) != 0)
      return -1;
    file->cur_cluster = next;
  }
  file->pos = position;
  return 0;
}

static u32 alloc_cluster(void) {
  for (u32 cluster = 2; cluster < cluster_limit; cluster++) {
    if (fat_get(cluster) == 0) {
      fat_set(cluster, cluster_eoc());
      memset(cluster_data(cluster), 0, cluster_bytes());
      return cluster;
    }
  }
  return 0;
}

static void free_chain(u32 first) {
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

/* Reserve `count` consecutive directory slots, extending the directory by
 * a cluster when it runs out. Consecutive matters: an LFN run and its short
 * entry must be adjacent, or nothing will pair them back up. */
static int alloc_dir_slots(struct fat_dir dir, u32 count,
                           struct dir_entry **out) {
  u32 run = 0;

  if (dir_is_fixed_root(dir)) {
    struct dir_entry *root = (struct dir_entry *)sector(root_start_sec);
    for (u32 i = 0; i < root_entries; i++) {
      if (entry_is_free(&root[i])) {
        out[run++] = &root[i];
        if (run == count)
          return 0;
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
        out[run++] = &entries[i];
        if (run == count)
          return 0;
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
      /* A fresh cluster is zeroed, so its slots read free and the run in
       * progress simply continues into it. */
      next = alloc_cluster();
      if (!next)
        return -1;
      fat_set(current, next);
    }
    current = next;
  }
}

/* Does any entry in `dir` already carry this raw 8.3 name? */
static int short_name_taken(struct fat_dir dir, const char name[FAT_NAME_LEN]) {
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, 0) != 0)
    return 0;

  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((u8)entry->name[0] == 0x00)
      return 0;
    if (!entry_is_usable(entry))
      continue;
    if (memcmp(entry->name, name, FAT_NAME_LEN) == 0)
      return 1;
  }
  return 0;
}

/* Build the "BASE~N.EXT" alias for a name that cannot be stored as 8.3.
 * Illegal characters collapse to '_' so the alias stays a legal short name
 * whatever the long name contains. */
static int short_name_alias(struct fat_dir dir, const char *name,
                            u32 length, char out[FAT_NAME_LEN]) {
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

    /* "~N" has to fit inside the 8-byte base, so the stem shrinks as the
     * ordinal grows. */
    u32 stem = 8 - (suffix_len + 1);
    if (stem > base_used)
      stem = base_used;

    memset(out, ' ', FAT_NAME_LEN);
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

static void write_lfn_slot(struct dir_entry *slot, const char *name,
                           u32 length, u8 seq, int last,
                           u8 checksum) {
  u8 *raw = (u8 *)slot;
  memset(raw, 0, sizeof(*slot));
  raw[0] = (u8)(seq | (last ? 0x40 : 0));
  raw[11] = FAT_ATTR_LFN;
  raw[13] = checksum;

  u32 base = (u32)(seq - 1) * FAT_LFN_CHARS_PER_SLOT;
  for (int i = 0; i < FAT_LFN_CHARS_PER_SLOT; i++) {
    u32 index = base + (u32)i;
    u16 ch;
    if (index < length)
      ch = (u16)(u8)name[index];
    else if (index == length)
      ch = 0x0000; /* terminator, only when the last slot is not full */
    else
      ch = 0xFFFF; /* padding */
    raw[lfn_offsets[i]] = (u8)(ch & 0xFF);
    raw[lfn_offsets[i] + 1] = (u8)(ch >> 8);
  }
}

/* Create one directory entry, with LFN slots in front of it when the name
 * needs them. `cluster` seeds first_cluster; 0 for an empty file. */
static int create_entry(struct fat_dir parent, const char *leaf, u8 attr,
                        u32 cluster, struct dir_entry **out) {
  u32 length = (u32)strlen(leaf);
  if (length == 0 || length > FAT_LFN_MAX)
    return -1;

  char short_name[FAT_NAME_LEN];
  u8 nt_case = 0;
  u32 lfn_slots = 0;

  if (short_name_exact(leaf, length, short_name, &nt_case) != 0) {
    if (short_name_alias(parent, leaf, length, short_name) != 0)
      return -1;
    lfn_slots = (length + FAT_LFN_CHARS_PER_SLOT - 1) / FAT_LFN_CHARS_PER_SLOT;
    nt_case = 0;
  }

  struct dir_entry *slots[FAT_MAX_SLOTS];
  if (alloc_dir_slots(parent, lfn_slots + 1, slots) != 0)
    return -1;

  /* Slots run highest sequence first, so slot[0] carries the last
   * fragment and the 0x40 marker. */
  if (lfn_slots) {
    u8 checksum = lfn_checksum(short_name);
    for (u32 i = 0; i < lfn_slots; i++) {
      u8 seq = (u8)(lfn_slots - i);
      write_lfn_slot(slots[i], leaf, length, seq, i == 0, checksum);
    }
  }

  struct dir_entry *entry = slots[lfn_slots];
  memset(entry, 0, sizeof(*entry));
  memcpy(entry->name, short_name, FAT_NAME_LEN);
  entry->attr = attr;
  entry->nt_case = nt_case;
  entry_set_cluster(entry, cluster);
  entry->size = 0;
  fat_set_timestamp(entry);

  /* Push the whole slot run, LFN fragments included. They are contiguous and
   * usually share a sector, so fat_flush_bytes collapses to one write; a run
   * straddling a sector boundary gets both. Flushing only the short entry
   * would leave a name on disk whose fragments were never written. */
  fat_flush_bytes(slots[0],
                  (usize)(lfn_slots + 1) * sizeof(struct dir_entry));

  *out = entry;
  return 0;
}

/* Drop an open file's contents while keeping its directory entry. The
 * alternative , unlink then create , destroys the entry before knowing a
 * replacement can be allocated, so a failure halfway leaves no file at
 * all. This cannot fail: freeing a chain never allocates. */
int fat_truncate(struct fat_file *file) {
  if (!file || !file->dir_ent)
    return -1;

  if (file->first_cluster)
    free_chain(file->first_cluster);

  file->first_cluster = 0;
  file->cur_cluster = 0;
  file->size = 0;
  file->pos = 0;
  entry_set_cluster(file->dir_ent, 0);
  file->dir_ent->size = 0;
  fat_set_timestamp(file->dir_ent);
  fat_flush_bytes(file->dir_ent, sizeof(*file->dir_ent));
  return 0;
}

int fat_create(const char *path, struct fat_file *file) {
  if (!file)
    return -1;
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  struct dir_slot existing;
  if (resolve_parent(path, &parent, leaf) != 0 ||
      find_in_directory(parent, leaf, &existing) == 0)
    return -1;

  struct dir_entry *entry;
  if (create_entry(parent, leaf, FAT_ATTR_ARCHIVE, 0, &entry) != 0)
    return -1;

  file->first_cluster = 0;
  file->cur_cluster = 0;
  file->size = 0;
  file->pos = 0;
  file->dir_ent = entry;
  return 0;
}

usize fat_write(struct fat_file *file, const void *buffer, usize length) {
  if (!file || !file->dir_ent || !buffer)
    return 0;
  const u8 *in = (const u8 *)buffer;
  usize total = 0;
  u32 bytes = cluster_bytes();

  if (file->first_cluster == 0) {
    u32 cluster = alloc_cluster();
    if (!cluster)
      return 0;
    file->first_cluster = cluster;
    file->cur_cluster = cluster;
    entry_set_cluster(file->dir_ent, cluster);
  }

  /* At an exact EOF boundary cur_cluster names the preceding cluster. */
  if (length > 0 && file->pos > 0 && file->pos == file->size &&
      file->pos % bytes == 0) {
    u32 next;
    int status = next_cluster(file->cur_cluster, &next);
    if (status == 1) {
      next = alloc_cluster();
      if (!next)
        return 0;
      fat_set(file->cur_cluster, next);
    } else if (status < 0) {
      return 0;
    }
    file->cur_cluster = next;
  }

  while (length > 0) {
    u8 *data = cluster_data(file->cur_cluster);
    if (!data)
      break;
    u32 offset = file->pos % bytes;
    u32 available = bytes - offset;
    usize chunk = length < available ? length : available;
    memcpy(data + offset, in, chunk);
    /* File data is the bulk of what a write changes and was never pushed
     * through: only the FAT and deleted directory entries were, so a saved
     * file survived only if something later called fat_flush(). */
    fat_flush_bytes(data + offset, chunk);

    in += chunk;
    file->pos += (u32)chunk;
    length -= chunk;
    total += chunk;

    if (file->pos % bytes == 0 && length > 0) {
      u32 next;
      int status = next_cluster(file->cur_cluster, &next);
      if (status == 1) {
        next = alloc_cluster();
        if (!next)
          break;
        fat_set(file->cur_cluster, next);
      } else if (status < 0) {
        break;
      }
      file->cur_cluster = next;
    }
  }

  if (file->pos > file->size) {
    file->size = file->pos;
    file->dir_ent->size = file->size;
  }
  /* The entry carries the size and, when this write allocated the first
   * cluster, the start cluster too. Flushed unconditionally: cheap, one
   * sector, and both fields change on the common path. */
  fat_set_timestamp(file->dir_ent);
  fat_flush_bytes(file->dir_ent, sizeof(*file->dir_ent));

  /* Preserve the seek invariant for another overwrite call: positions
   * inside the file point at the cluster containing the next byte. */
  if (file->pos > 0 && file->pos < file->size && file->pos % bytes == 0) {
    u32 next;
    if (next_cluster(file->cur_cluster, &next) == 0)
      file->cur_cluster = next;
  }
  return total;
}

// /* Erase a slot range, LFN entries included. Leaving the LFN slots behind
//  * would strand a run whose short entry no longer exists, and the next
//  * created file would inherit that name. */
// static void erase_slots(struct fat_dir dir, u32 from, u32 to) {
//   struct dir_cursor cursor;
//   if (cursor_init(&cursor, dir, from) != 0)
//     return;

//   struct dir_entry *entry;
//   while (cursor.index <= to && (entry = cursor_next(&cursor)) != 0)
//     entry->name[0] = (char)0xE5;
// }

int fat_unlink(const char *path) {
  struct fat_dir parent;
  struct dir_slot slot;
  if (resolve_slot(path, &parent, &slot) != 0)
    return -1;
  if (slot.entry->attr & FAT_ATTR_DIRECTORY)
    return -1;

  u32 cluster = entry_cluster(slot.entry);
  if (cluster)
    free_chain(cluster);
  erase_slots(parent, slot.lfn_index, slot.index);
  return 0;
}

static void set_dot_entry(struct dir_entry *entry, int parent,
                          u32 cluster) {
  memset(entry, 0, sizeof(*entry));
  memset(entry->name, ' ', FAT_NAME_LEN);
  entry->name[0] = '.';
  if (parent)
    entry->name[1] = '.';
  entry->attr = FAT_ATTR_DIRECTORY;
  entry_set_cluster(entry, cluster);
  /* Stamp these too , a freshly created directory whose own '.' and '..'
   * read 1980-00-00 looks like a damaged entry next to the dated one the
   * parent holds. */
  fat_set_timestamp(entry);
}

int fat_mkdir(const char *path) {
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  struct dir_slot existing;
  if (resolve_parent(path, &parent, leaf) != 0 ||
      find_in_directory(parent, leaf, &existing) == 0)
    return -1;

  u32 cluster = alloc_cluster();
  if (!cluster)
    return -1;
  struct dir_entry *children = (struct dir_entry *)cluster_data(cluster);
  if (!children) {
    free_chain(cluster);
    return -1;
  }

  /* alloc_cluster hands back whatever the chain had; the rest of this cluster
   * has to read as free slots or a scan of the new directory would walk into
   * stale entries. */
  memset(children, 0, cluster_bytes());
  set_dot_entry(&children[0], 0, cluster);
  /* ".." of a directory whose parent is the root is stored as cluster 0,
   * on FAT32 too -- the root cluster number is deliberately not used. */
  set_dot_entry(&children[1], 1, parent.is_root ? 0 : parent.first_cluster);

  struct dir_entry *entry;
  if (create_entry(parent, leaf, FAT_ATTR_DIRECTORY, cluster, &entry) != 0) {
    free_chain(cluster);
    return -1;
  }

  /* Only after create_entry succeeds , flushing first would put a directory
   * on disk that nothing references if the entry allocation then failed. */
  fat_flush_bytes(children, cluster_bytes());
  return 0;
}

static int is_dot_entry(const struct dir_entry *entry) {
  return entry->name[0] == '.' &&
         (entry->name[1] == ' ' || entry->name[1] == '.');
}

static int path_ends_with_dot_component(const char *path) {
  u32 length = 0;
  if (!path)
    return 0;
  while (length < FAT_PATH_MAX && path[length])
    length++;
  if (length == FAT_PATH_MAX)
    return 1;

  while (length > 0 && is_separator(path[length - 1]))
    length--;
  u32 start = length;
  while (start > 0 && !is_separator(path[start - 1]))
    start--;
  u32 component_length = length - start;
  return (component_length == 1 && path[start] == '.') ||
         (component_length == 2 && path[start] == '.' &&
          path[start + 1] == '.');
}

int fat_rmdir(const char *path) {
  struct fat_dir parent;
  struct dir_slot slot;
  if (path_ends_with_dot_component(path) ||
      resolve_slot(path, &parent, &slot) != 0 ||
      !(slot.entry->attr & FAT_ATTR_DIRECTORY) || is_dot_entry(slot.entry))
    return -1;

  u32 cluster = entry_cluster(slot.entry);
  if (!cluster_is_valid(cluster))
    return -1;

  struct fat_dir child = {.first_cluster = cluster, .is_root = 0};
  struct dir_cursor cursor;
  if (cursor_init(&cursor, child, 0) != 0)
    return -1;

  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((u8)entry->name[0] == 0x00)
      break;
    if (entry_is_usable(entry) && !is_dot_entry(entry))
      return -1;
  }

  free_chain(cluster);
  erase_slots(parent, slot.lfn_index, slot.index);
  return 0;
}

/* FAT packs a date into 16 bits as year-since-1980:7 | month:4 | day:5, and a
 * time as hour:5 | minute:6 | (second/2):5 , two-second resolution is the
 * format's, not ours. */
static u16 fat_encode_date(const struct rtc_time *t) {
  u16 year = (u16)(t->year - 1980);
  return (u16)((year << 9) | ((u16)t->month << 5) | t->day);
}

static u16 fat_encode_time(const struct rtc_time *t) {
  return (u16)(((u16)t->hour << 11) | ((u16)t->minute << 5) |
                    (t->second / 2));
}

void fat_set_timestamp(struct dir_entry *entry) {
  if (!entry)
    return;

  struct rtc_time now;
  rtc_read(&now);
  /* Leave the fields untouched when the clock is unreadable: a zeroed date is
   * what the entry already carries, and writing an out-of-range one would
   * make host tools report the volume as damaged rather than undated. */
  if (!now.valid)
    return;

  u16 date = fat_encode_date(&now);
  u16 time = fat_encode_time(&now);

  entry->write_date = date;
  entry->write_time = time;
  entry->access_date = date;

  /* First stamp doubles as the creation stamp. Callers hit this on create and
   * again on every write, and only the creation path finds it unset. */
  if (entry->create_date == 0 && entry->create_time == 0) {
    entry->create_date = date;
    entry->create_time = time;
    entry->create_time_tenth = 0;
  }
}

long fat_read_dir(const char *path, u32 *index, char *buffer,
                  usize length) {
  if (!index || !buffer || length == 0)
    return -1;
  struct fat_dir dir;
  if (resolve_directory(path, &dir) != 0)
    return -1;

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
    char name[FAT_DIRENT_MAX];
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
      /* Resume at the head of this entry's LFN run, not at its short
       * entry: restarting mid-run would lose the long name. */
      cursor.index = long_name ? lfn.start_index : cursor.index - 1;
      break;
    }
    memcpy(buffer + written, name, required);
    written += required;
    lfn_reset(&lfn);
  }
  *index = cursor.index;
  return (long)written;
}

long fat_read_root_dir(u32 *index, char *buffer, usize length) {
  return fat_read_dir("/", index, buffer, length);
}

long fat_read_dir_one(const char *path, u32 *index, char *buffer,
                      usize length, int *is_dir) {
  if (!index || !buffer || length == 0)
    return -1;

  struct fat_dir dir;
  if (resolve_directory(path, &dir) != 0)
    return -1;

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

void fat_set_sector_writer(fat_sector_writer writer) { sector_writer = writer; }

usize fat_volume_size(const void *boot_sector, u32 *bytes_per_sector_out) {
  if (bytes_per_sector_out)
    *bytes_per_sector_out = 0;
  if (!boot_sector)
    return 0;

  const struct bpb *b = (const struct bpb *)boot_sector;
  u32 total = b->total_sectors_16 ? b->total_sectors_16
                                       : b->total_sectors_32;
  if (!total || !b->bytes_per_sector)
    return 0;

  if (bytes_per_sector_out)
    *bytes_per_sector_out = b->bytes_per_sector;
  return (usize)total * b->bytes_per_sector;
}

u8 *fat_image_base(usize *size_out, u32 *bytes_per_sector_out) {
  if (size_out)
    *size_out = fs_image ? fs_image_size : 0;
  if (bytes_per_sector_out)
    *bytes_per_sector_out = fs_image ? bytes_per_sec : 0;
  return fs_image;
}
