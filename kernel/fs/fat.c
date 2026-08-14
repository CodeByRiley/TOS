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
#include "drivers/storage/ahci.h"
#include "memory/heap.h"
#include "memory/hhdm.h"
#include <fs/fat.h>
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
// FAT_ATTR_LFN is not a bit but the value 0x0F — all four of the low bits at
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

struct __attribute__((packed)) bpb {
  uint8_t jmp[3];
  char oem[8];
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t num_fats;
  uint16_t root_entries;
  uint16_t total_sectors_16;
  uint8_t media;
  uint16_t sectors_per_fat_16;
  uint16_t sectors_per_track;
  uint16_t heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors_32;
  /* FAT32 extended block. Only meaningful when sectors_per_fat_16 is 0. */
  uint32_t sectors_per_fat_32;
  uint16_t ext_flags;
  uint16_t fs_version;
  uint32_t root_cluster;
  uint16_t fs_info_sector;
  uint16_t backup_boot_sector;
};

_Static_assert(sizeof(struct bpb) == 52, "BPB layout must match on-disk");

struct __attribute__((packed)) dir_entry {
  char name[8];
  char ext[3];
  uint8_t attr;
  uint8_t nt_case;
  uint8_t create_time_tenth;
  uint16_t create_time;
  uint16_t create_date;
  uint16_t access_date;
  uint16_t first_cluster_high;
  uint16_t write_time;
  uint16_t write_date;
  uint16_t first_cluster_low;
  uint32_t size;
};

_Static_assert(sizeof(struct dir_entry) == 32,
               "FAT directory entries must be 32 bytes");

struct fat_dir {
  uint32_t first_cluster;
  int is_root;
};

struct path_parts {
  const char *component[FAT_MAX_COMPONENTS];
  uint8_t length[FAT_MAX_COMPONENTS];
  uint8_t count;
};

struct dir_cursor {
  struct fat_dir dir;
  uint32_t cluster;
  uint32_t slot;
  uint32_t index;
  uint32_t clusters_seen;
  int ended;
};

/* A resolved directory entry plus the slot range it occupies. `index` is
 * the cursor index of the short entry; `lfn_index` is where its LFN run
 * starts (equal to `index` when the entry has no long name). Deleting an
 * entry has to erase the whole range, not just the short slot. */
struct dir_slot {
  struct dir_entry *entry;
  uint32_t index;
  uint32_t lfn_index;
};

/* Long-name accumulator. LFN slots precede their short entry and are
 * stored last-fragment-first, so a run is only trustworthy once the short
 * entry arrives and its checksum matches. */
struct lfn_state {
  char name[FAT_LFN_BUFFER];
  uint32_t length;
  uint32_t start_index;
  uint8_t checksum;
  uint8_t expect;
  uint8_t valid;
};

static uint8_t *fs_image;
static size_t fs_image_size;
static uint16_t bytes_per_sec;
static uint8_t sec_per_clus;
static uint8_t num_fats;
static uint32_t sectors_per_fat;
static uint32_t fat_start_sec;
static uint32_t root_start_sec;
static uint32_t data_start_sec;
static uint16_t root_entries;
static uint32_t root_cluster;
static uint32_t fsinfo_sec;
static enum fat_type fat_type;
/* Exclusive upper bound; valid data clusters are [2, cluster_limit). */
static uint32_t cluster_limit;

extern struct AHCI_DEVICE_DATA *g_ahci_dev;

/* Byte offsets of the 13 name characters inside an LFN slot. */
static const uint8_t lfn_offsets[FAT_LFN_CHARS_PER_SLOT] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static uint32_t cluster_bytes(void) {
  return (uint32_t)sec_per_clus * bytes_per_sec;
}

static uint8_t *sector(uint32_t lba) {
  return fs_image + (uint64_t)lba * bytes_per_sec;
}

static int cluster_is_valid(uint32_t cluster) {
  return cluster >= 2 && cluster < cluster_limit;
}

static uint32_t cluster_eoc(void) {
  return fat_type == FAT_TYPE_32 ? 0x0FFFFFFFu : 0xFFFFu;
}

static int cluster_is_eoc(uint32_t value) {
  return fat_type == FAT_TYPE_32 ? value >= 0x0FFFFFF8u : value >= 0xFFF8u;
}

static int cluster_is_bad(uint32_t value) {
  return fat_type == FAT_TYPE_32 ? value == 0x0FFFFFF7u : value == 0xFFF7u;
}

/* FAT16 entries are 16 bits, FAT32 entries 28 bits inside a 32-bit slot. */
static uint32_t fat_get(uint32_t cluster) {
  uint8_t *table = sector(fat_start_sec);
  if (fat_type == FAT_TYPE_32)
    return ((uint32_t *)table)[cluster] & 0x0FFFFFFFu;
  return ((uint16_t *)table)[cluster];
}

/* The FAT32 entry's top 4 bits are reserved and must survive a write. */
static void fat_put(uint8_t *table, uint32_t cluster, uint32_t value) {
  if (fat_type == FAT_TYPE_32) {
    uint32_t *slots = (uint32_t *)table;
    slots[cluster] = (slots[cluster] & 0xF0000000u) | (value & 0x0FFFFFFFu);
  } else {
    ((uint16_t *)table)[cluster] = (uint16_t)value;
  }
}

/* The free-cluster counters cached in FSInfo go stale the moment we
 * allocate. Rather than track them, mark them unknown so a host that
 * mounts this image recomputes instead of trusting our arithmetic. */
static void fsinfo_invalidate(void) {
  if (!fsinfo_sec)
    return;
  uint8_t *info = sector(fsinfo_sec);
  if (*(uint32_t *)info != 0x41615252u)
    return;
  *(uint32_t *)(info + 488) = 0xFFFFFFFFu; /* free cluster count  */
  *(uint32_t *)(info + 492) = 0xFFFFFFFFu; /* next-free-cluster hint */
}

/* Return 0 for a valid successor, 1 for EOC, and -1 for corruption. */
static int next_cluster(uint32_t cluster, uint32_t *next) {
  if (!cluster_is_valid(cluster))
    return -1;
  uint32_t value = fat_get(cluster);
  if (cluster_is_eoc(value))
    return 1;
  if (cluster_is_bad(value) || !cluster_is_valid(value))
    return -1;
  *next = value;
  return 0;
}

static uint8_t *cluster_data(uint32_t cluster) {
  if (!cluster_is_valid(cluster))
    return 0;
  uint32_t lba = data_start_sec + (cluster - 2) * sec_per_clus;
  return sector(lba);
}

/* FAT16 keeps first_cluster_high reserved and zero; only FAT32 uses it. */
static uint32_t entry_cluster(const struct dir_entry *entry) {
  uint32_t low = entry->first_cluster_low;
  if (fat_type != FAT_TYPE_32)
    return low;
  return low | ((uint32_t)entry->first_cluster_high << 16);
}

static void entry_set_cluster(struct dir_entry *entry, uint32_t cluster) {
  entry->first_cluster_low = (uint16_t)(cluster & 0xFFFF);
  entry->first_cluster_high =
      fat_type == FAT_TYPE_32 ? (uint16_t)(cluster >> 16) : 0;
}

int fat_type_bits(void) { return (int)fat_type; }

int fat_init(uint8_t *image, size_t size) {
  if (!image || size < 512)
    return -1;

  struct bpb *b = (struct bpb *)image;
  uint32_t total_sectors =
      b->total_sectors_16 ? b->total_sectors_16 : b->total_sectors_32;
  uint32_t fat_sectors =
      b->sectors_per_fat_16 ? b->sectors_per_fat_16 : b->sectors_per_fat_32;
  if (b->bytes_per_sector < 512 || b->bytes_per_sector > 4096 ||
      (b->bytes_per_sector & (b->bytes_per_sector - 1)) != 0 ||
      b->sectors_per_cluster == 0 || b->reserved_sectors == 0 ||
      b->num_fats == 0 || fat_sectors == 0 || total_sectors == 0 ||
      (uint64_t)total_sectors * b->bytes_per_sector > size)
    return -1;

  uint32_t root_sectors =
      ((uint32_t)b->root_entries * sizeof(struct dir_entry) +
       b->bytes_per_sector - 1) /
      b->bytes_per_sector;
  uint32_t fat_start = b->reserved_sectors;
  uint32_t root_start = fat_start + (uint32_t)b->num_fats * fat_sectors;
  uint32_t data_start = root_start + root_sectors;
  if (root_start < fat_start || data_start < root_start ||
      data_start >= total_sectors)
    return -1;

  uint32_t data_clusters =
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

  uint32_t entry_bytes = type == FAT_TYPE_32 ? 4 : 2;
  uint32_t fat_entries =
      (uint32_t)((uint64_t)fat_sectors * b->bytes_per_sector / entry_bytes);
  uint32_t limit = data_clusters + 2;
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
  log_write_hex("FAT: type          =", (uint64_t)fat_type, KERNEL, LOG_INFO);
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

  uint32_t pos = 0;
  for (;;) {
    while (pos < FAT_PATH_MAX && is_separator(path[pos]))
      pos++;
    if (pos >= FAT_PATH_MAX)
      return -1;
    if (path[pos] == '\0')
      return 0;

    uint32_t start = pos;
    while (pos < FAT_PATH_MAX && path[pos] != '\0' && !is_separator(path[pos]))
      pos++;
    if (pos >= FAT_PATH_MAX)
      return -1;

    uint32_t length = pos - start;
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
    parts->length[parts->count] = (uint8_t)length;
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
static void split_extension(const char *name, uint32_t length,
                            uint32_t *base_len, uint32_t *ext_at,
                            uint32_t *ext_len) {
  uint32_t dot = length;
  for (uint32_t i = length; i > 1; i--) {
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
static int short_name_exact(const char *name, uint32_t length,
                            char out[FAT_NAME_LEN], uint8_t *nt_case) {
  /* A trailing dot has no 8.3 spelling: the extension field would be
   * empty and the name would read back without the dot. */
  if (length == 0 || length > 12 || name[length - 1] == '.')
    return -1;

  uint32_t base_len, ext_at, ext_len;
  split_extension(name, length, &base_len, &ext_at, &ext_len);
  if (base_len == 0 || base_len > 8 || ext_len > 3)
    return -1;

  int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
  for (uint32_t i = 0; i < base_len; i++) {
    if (!valid_short_char(name[i]))
      return -1;
    if (is_lower(name[i]))
      base_lower = 1;
    if (is_upper(name[i]))
      base_upper = 1;
  }
  for (uint32_t i = 0; i < ext_len; i++) {
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
  for (uint32_t i = 0; i < base_len; i++)
    out[i] = to_upper(name[i]);
  for (uint32_t i = 0; i < ext_len; i++)
    out[8 + i] = to_upper(name[ext_at + i]);

  /* 0xE5 is the deleted marker; the standard escape is 0x05. */
  if ((uint8_t)out[0] == 0xE5)
    out[0] = 0x05;

  *nt_case = (uint8_t)((base_lower ? FAT_CASE_BASE_LOWER : 0) |
                       (ext_lower ? FAT_CASE_EXT_LOWER : 0));
  return 0;
}

/* Checksum tying an LFN run to its short entry. Any edit to the short name
 * by an 8.3-only writer breaks it, which is exactly how such a writer
 * signals that the long name is now stale. */
static uint8_t lfn_checksum(const char *name) {
  uint8_t sum = 0;
  for (int i = 0; i < FAT_NAME_LEN; i++)
    sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)name[i]);
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
                       uint32_t start_index) {
  memset(cursor, 0, sizeof(*cursor));
  cursor->dir = dir;
  cursor->index = start_index;

  if (dir_is_fixed_root(dir)) {
    cursor->slot = start_index;
    return start_index <= root_entries ? 0 : -1;
  }
  if (!cluster_is_valid(dir.first_cluster))
    return -1;

  uint32_t entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
  uint32_t clusters_to_skip = start_index / entries_per_cluster;
  cursor->slot = start_index % entries_per_cluster;
  cursor->cluster = dir.first_cluster;
  cursor->clusters_seen = 1;

  while (clusters_to_skip--) {
    uint32_t next;
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

  uint32_t entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
  if (cursor->slot >= entries_per_cluster) {
    uint32_t next;
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

/* Writes a single 512-byte sector from the RAM array back to the physical disk */
static void fat_flush_sector(uint32_t lba) {
    if (!fs_image || !g_ahci_dev) return;

    uint64_t virt = (uint64_t)sector(lba);
    uint64_t phys = virt_to_phys((void*)virt);

    ahci_write_sector(g_ahci_dev, 0, lba, 1, (void*)phys);
}

static void erase_slots(struct fat_dir dir, uint32_t from, uint32_t to) {
  struct dir_cursor cursor;
  if (cursor_init(&cursor, dir, from) != 0)
    return;

  struct dir_entry *entry;
  while (cursor.index <= to && (entry = cursor_next(&cursor)) != 0) {
    entry->name[0] = (char)0xE5; // Mark as deleted

    // Calculate the LBA of the sector containing this entry and flush it!
    // entry is a pointer inside fs_image.
    // (entry - fs_image) gives the byte offset. Divide by bytes_per_sec to get LBA.
    uint64_t byte_offset = (uint64_t)entry - (uint64_t)fs_image;
    uint32_t lba = byte_offset / bytes_per_sec;
    fat_flush_sector(lba);
  }
}

static int entry_is_lfn(const struct dir_entry *entry) {
  return (entry->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN;
}

/* Mirror writes to every FAT copy. */
static void fat_set(uint32_t cluster, uint32_t value) {
  for (uint8_t f = 0; f < num_fats; f++) {
    uint8_t *table = sector(fat_start_sec + f * sectors_per_fat);
    fat_put(table, cluster, value);

    // Flush the specific FAT sector that was modified
    uint64_t byte_offset = (uint64_t)table - (uint64_t)fs_image;
    uint32_t lba = byte_offset / bytes_per_sec;
    fat_flush_sector(lba);
  }
  fsinfo_invalidate();
  // Also flush the FSInfo sector if it exists!
  if (fsinfo_sec) fat_flush_sector(fsinfo_sec);
}

static int entry_is_free(const struct dir_entry *entry) {
  uint8_t first = (uint8_t)entry->name[0];
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
                     uint32_t index) {
  const uint8_t *raw = (const uint8_t *)entry;
  uint8_t marker = raw[0];

  if (marker == 0xE5) {
    lfn_reset(state);
    return;
  }

  uint8_t seq = marker & 0x1F;
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
    state->length = (uint32_t)seq * FAT_LFN_CHARS_PER_SLOT;
    state->start_index = index;
  } else if (!state->valid || seq != state->expect || raw[13] != state->checksum) {
    lfn_reset(state);
    return;
  }

  uint32_t base = (uint32_t)(seq - 1) * FAT_LFN_CHARS_PER_SLOT;
  for (int i = 0; i < FAT_LFN_CHARS_PER_SLOT; i++) {
    uint16_t ch =
        (uint16_t)(raw[lfn_offsets[i]] | ((uint16_t)raw[lfn_offsets[i] + 1] << 8));
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

  state->expect = (uint8_t)(seq - 1);
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
  uint32_t length = 0;
  while (length < state->length && state->name[length] != '\0')
    length++;
  if (length == 0 || length > FAT_LFN_MAX)
    return 0;
  state->name[length] = '\0';
  return state->name;
}

/* Render an entry's 8.3 name for display, honouring the NT case flags. */
static uint32_t entry_short_name(const struct dir_entry *entry, char *out) {
  uint32_t length = 0;
  int base_lower = (entry->nt_case & FAT_CASE_BASE_LOWER) != 0;
  int ext_lower = (entry->nt_case & FAT_CASE_EXT_LOWER) != 0;

  for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
    char c = entry->name[i];
    if (i == 0 && (uint8_t)c == 0x05)
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
    if ((uint8_t)entry->name[0] == 0x00)
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

static int walk_directories(const struct path_parts *parts, uint32_t count,
                            struct fat_dir *out) {
  struct fat_dir current = root_directory();
  for (uint32_t i = 0; i < count; i++) {
    char component[FAT_LFN_MAX + 1];
    memcpy(component, parts->component[i], parts->length[i]);
    component[parts->length[i]] = '\0';

    struct dir_slot slot;
    if (find_in_directory(current, component, &slot) != 0)
      return -1;
    if (!(slot.entry->attr & FAT_ATTR_DIRECTORY))
      return -1;

    uint32_t cluster = entry_cluster(slot.entry);
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

  uint32_t last = (uint32_t)parts.count - 1;
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

  uint32_t cluster = entry_cluster(entry);
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

size_t fat_read(struct fat_file *file, void *buffer, size_t length) {
  if (!file || !buffer)
    return 0;
  uint8_t *out = (uint8_t *)buffer;
  size_t total = 0;
  uint32_t bytes = cluster_bytes();

  while (length > 0 && file->pos < file->size) {
    uint8_t *data = cluster_data(file->cur_cluster);
    if (!data)
      break;
    uint32_t offset = file->pos % bytes;
    uint32_t in_cluster = bytes - offset;
    uint32_t in_file = file->size - file->pos;
    size_t chunk = length;
    if (chunk > in_cluster)
      chunk = in_cluster;
    if (chunk > in_file)
      chunk = in_file;

    memcpy(out, data + offset, chunk);
    out += chunk;
    file->pos += (uint32_t)chunk;
    length -= chunk;
    total += chunk;

    if (file->pos % bytes == 0 && file->pos < file->size) {
      uint32_t next;
      if (next_cluster(file->cur_cluster, &next) != 0)
        break;
      file->cur_cluster = next;
    }
  }
  return total;
}

int fat_seek(struct fat_file *file, uint32_t position) {
  if (!file)
    return -1;
  if (position > file->size)
    position = file->size;
  file->cur_cluster = file->first_cluster;
  if (position == 0 || file->size == 0) {
    file->pos = position;
    return 0;
  }

  uint32_t bytes = cluster_bytes();
  uint32_t skip = position / bytes;
  /* At an exact EOF boundary there is no containing cluster. Keep the
   * cursor on the final cluster so a subsequent append can extend it. */
  if (position == file->size && position % bytes == 0)
    skip--;
  while (skip--) {
    uint32_t next;
    if (next_cluster(file->cur_cluster, &next) != 0)
      return -1;
    file->cur_cluster = next;
  }
  file->pos = position;
  return 0;
}

static uint32_t alloc_cluster(void) {
  for (uint32_t cluster = 2; cluster < cluster_limit; cluster++) {
    if (fat_get(cluster) == 0) {
      fat_set(cluster, cluster_eoc());
      memset(cluster_data(cluster), 0, cluster_bytes());
      return cluster;
    }
  }
  return 0;
}

static void free_chain(uint32_t first) {
  uint32_t current = first;
  uint32_t visited = 0;
  while (cluster_is_valid(current) && visited++ < cluster_limit) {
    uint32_t next = fat_get(current);
    fat_set(current, 0);
    if (cluster_is_eoc(next) || !cluster_is_valid(next))
      break;
    current = next;
  }
}

/* Reserve `count` consecutive directory slots, extending the directory by
 * a cluster when it runs out. Consecutive matters: an LFN run and its short
 * entry must be adjacent, or nothing will pair them back up. */
static int alloc_dir_slots(struct fat_dir dir, uint32_t count,
                           struct dir_entry **out) {
  uint32_t run = 0;

  if (dir_is_fixed_root(dir)) {
    struct dir_entry *root = (struct dir_entry *)sector(root_start_sec);
    for (uint32_t i = 0; i < root_entries; i++) {
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

  uint32_t current = dir.first_cluster;
  if (!cluster_is_valid(current))
    return -1;
  uint32_t entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
  uint32_t visited = 0;

  for (;;) {
    struct dir_entry *entries = (struct dir_entry *)cluster_data(current);
    if (!entries)
      return -1;
    for (uint32_t i = 0; i < entries_per_cluster; i++) {
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

    uint32_t next;
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
    if ((uint8_t)entry->name[0] == 0x00)
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
                            uint32_t length, char out[FAT_NAME_LEN]) {
  uint32_t base_len, ext_at, ext_len;
  split_extension(name, length, &base_len, &ext_at, &ext_len);

  char base[8];
  uint32_t base_used = 0;
  for (uint32_t i = 0; i < base_len && base_used < 8; i++) {
    char c = name[i];
    if (c == ' ' || c == '.')
      continue;
    base[base_used++] = valid_short_char(c) ? to_upper(c) : '_';
  }
  if (base_used == 0)
    base[base_used++] = '_';

  char ext[3];
  uint32_t ext_used = 0;
  for (uint32_t i = 0; i < ext_len && ext_used < 3; i++) {
    char c = name[ext_at + i];
    if (c == ' ' || c == '.')
      continue;
    ext[ext_used++] = valid_short_char(c) ? to_upper(c) : '_';
  }

  for (uint32_t n = 1; n <= 999999; n++) {
    char suffix[7];
    uint32_t suffix_len = 0;
    for (uint32_t value = n; value; value /= 10)
      suffix[suffix_len++] = (char)('0' + value % 10);

    /* "~N" has to fit inside the 8-byte base, so the stem shrinks as the
     * ordinal grows. */
    uint32_t stem = 8 - (suffix_len + 1);
    if (stem > base_used)
      stem = base_used;

    memset(out, ' ', FAT_NAME_LEN);
    for (uint32_t i = 0; i < stem; i++)
      out[i] = base[i];
    out[stem] = '~';
    for (uint32_t i = 0; i < suffix_len; i++)
      out[stem + 1 + i] = suffix[suffix_len - 1 - i];
    for (uint32_t i = 0; i < ext_used; i++)
      out[8 + i] = ext[i];

    if (!short_name_taken(dir, out))
      return 0;
  }
  return -1;
}

static void write_lfn_slot(struct dir_entry *slot, const char *name,
                           uint32_t length, uint8_t seq, int last,
                           uint8_t checksum) {
  uint8_t *raw = (uint8_t *)slot;
  memset(raw, 0, sizeof(*slot));
  raw[0] = (uint8_t)(seq | (last ? 0x40 : 0));
  raw[11] = FAT_ATTR_LFN;
  raw[13] = checksum;

  uint32_t base = (uint32_t)(seq - 1) * FAT_LFN_CHARS_PER_SLOT;
  for (int i = 0; i < FAT_LFN_CHARS_PER_SLOT; i++) {
    uint32_t index = base + (uint32_t)i;
    uint16_t ch;
    if (index < length)
      ch = (uint16_t)(uint8_t)name[index];
    else if (index == length)
      ch = 0x0000; /* terminator, only when the last slot is not full */
    else
      ch = 0xFFFF; /* padding */
    raw[lfn_offsets[i]] = (uint8_t)(ch & 0xFF);
    raw[lfn_offsets[i] + 1] = (uint8_t)(ch >> 8);
  }
}

/* Create one directory entry, with LFN slots in front of it when the name
 * needs them. `cluster` seeds first_cluster; 0 for an empty file. */
static int create_entry(struct fat_dir parent, const char *leaf, uint8_t attr,
                        uint32_t cluster, struct dir_entry **out) {
  uint32_t length = (uint32_t)strlen(leaf);
  if (length == 0 || length > FAT_LFN_MAX)
    return -1;

  char short_name[FAT_NAME_LEN];
  uint8_t nt_case = 0;
  uint32_t lfn_slots = 0;

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
    uint8_t checksum = lfn_checksum(short_name);
    for (uint32_t i = 0; i < lfn_slots; i++) {
      uint8_t seq = (uint8_t)(lfn_slots - i);
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

  *out = entry;
  return 0;
}

/* Drop an open file's contents while keeping its directory entry. The
 * alternative — unlink then create — destroys the entry before knowing a
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

size_t fat_write(struct fat_file *file, const void *buffer, size_t length) {
  if (!file || !file->dir_ent || !buffer)
    return 0;
  const uint8_t *in = (const uint8_t *)buffer;
  size_t total = 0;
  uint32_t bytes = cluster_bytes();

  if (file->first_cluster == 0) {
    uint32_t cluster = alloc_cluster();
    if (!cluster)
      return 0;
    file->first_cluster = cluster;
    file->cur_cluster = cluster;
    entry_set_cluster(file->dir_ent, cluster);
  }

  /* At an exact EOF boundary cur_cluster names the preceding cluster. */
  if (length > 0 && file->pos > 0 && file->pos == file->size &&
      file->pos % bytes == 0) {
    uint32_t next;
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
    uint8_t *data = cluster_data(file->cur_cluster);
    if (!data)
      break;
    uint32_t offset = file->pos % bytes;
    uint32_t available = bytes - offset;
    size_t chunk = length < available ? length : available;
    memcpy(data + offset, in, chunk);

    in += chunk;
    file->pos += (uint32_t)chunk;
    length -= chunk;
    total += chunk;

    if (file->pos % bytes == 0 && length > 0) {
      uint32_t next;
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
  /* Preserve the seek invariant for another overwrite call: positions
   * inside the file point at the cluster containing the next byte. */
  if (file->pos > 0 && file->pos < file->size && file->pos % bytes == 0) {
    uint32_t next;
    if (next_cluster(file->cur_cluster, &next) == 0)
      file->cur_cluster = next;
  }
  return total;
}

// /* Erase a slot range, LFN entries included. Leaving the LFN slots behind
//  * would strand a run whose short entry no longer exists, and the next
//  * created file would inherit that name. */
// static void erase_slots(struct fat_dir dir, uint32_t from, uint32_t to) {
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

  uint32_t cluster = entry_cluster(slot.entry);
  if (cluster)
    free_chain(cluster);
  erase_slots(parent, slot.lfn_index, slot.index);
  return 0;
}

static void set_dot_entry(struct dir_entry *entry, int parent,
                          uint32_t cluster) {
  memset(entry, 0, sizeof(*entry));
  memset(entry->name, ' ', FAT_NAME_LEN);
  entry->name[0] = '.';
  if (parent)
    entry->name[1] = '.';
  entry->attr = FAT_ATTR_DIRECTORY;
  entry_set_cluster(entry, cluster);
}

int fat_mkdir(const char *path) {
  struct fat_dir parent;
  char leaf[FAT_LFN_MAX + 1];
  struct dir_slot existing;
  if (resolve_parent(path, &parent, leaf) != 0 ||
      find_in_directory(parent, leaf, &existing) == 0)
    return -1;

  uint32_t cluster = alloc_cluster();
  if (!cluster)
    return -1;
  struct dir_entry *children = (struct dir_entry *)cluster_data(cluster);
  set_dot_entry(&children[0], 0, cluster);
  /* ".." of a directory whose parent is the root is stored as cluster 0,
   * on FAT32 too -- the root cluster number is deliberately not used. */
  set_dot_entry(&children[1], 1, parent.is_root ? 0 : parent.first_cluster);

  struct dir_entry *entry;
  if (create_entry(parent, leaf, FAT_ATTR_DIRECTORY, cluster, &entry) != 0) {
    free_chain(cluster);
    return -1;
  }
  return 0;
}

static int is_dot_entry(const struct dir_entry *entry) {
  return entry->name[0] == '.' &&
         (entry->name[1] == ' ' || entry->name[1] == '.');
}

static int path_ends_with_dot_component(const char *path) {
  uint32_t length = 0;
  if (!path)
    return 0;
  while (length < FAT_PATH_MAX && path[length])
    length++;
  if (length == FAT_PATH_MAX)
    return 1;

  while (length > 0 && is_separator(path[length - 1]))
    length--;
  uint32_t start = length;
  while (start > 0 && !is_separator(path[start - 1]))
    start--;
  uint32_t component_length = length - start;
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

  uint32_t cluster = entry_cluster(slot.entry);
  if (!cluster_is_valid(cluster))
    return -1;

  struct fat_dir child = {.first_cluster = cluster, .is_root = 0};
  struct dir_cursor cursor;
  if (cursor_init(&cursor, child, 0) != 0)
    return -1;

  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((uint8_t)entry->name[0] == 0x00)
      break;
    if (entry_is_usable(entry) && !is_dot_entry(entry))
      return -1;
  }

  free_chain(cluster);
  erase_slots(parent, slot.lfn_index, slot.index);
  return 0;
}

void fat_set_timestamp(struct dir_entry *entry) {

}

long fat_read_dir(const char *path, uint32_t *index, char *buffer,
                  size_t length) {
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

  size_t written = 0;
  struct dir_entry *entry;
  while ((entry = cursor_next(&cursor)) != 0) {
    if ((uint8_t)entry->name[0] == 0x00)
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
    size_t name_length;
    if (long_name) {
      name_length = strlen(long_name);
      memcpy(name, long_name, name_length);
    } else {
      name_length = entry_short_name(entry, name);
    }
    if (entry->attr & FAT_ATTR_DIRECTORY)
      name[name_length++] = '/';
    name[name_length] = '\0';

    size_t required = name_length + 1;
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

long fat_read_root_dir(uint32_t *index, char *buffer, size_t length) {
  return fat_read_dir("/", index, buffer, length);
}

long fat_read_dir_one(const char *path, uint32_t *index, char *buffer,
                      size_t length, int *is_dir) {
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
    if ((uint8_t)entry->name[0] == 0x00)
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
    size_t name_length;
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

/* Load the physical AHCI disk into the FAT driver's RAM array */
int fat_mount_from_ahci(struct AHCI_DEVICE_DATA *ahci_dev, int port) {
    if (!ahci_dev) return -1;

    // 1. Read the boot sector (LBA 0) to find out how big the disk is
    uint8_t *bpb_buf = kmalloc(512);
    if (!bpb_buf) return -1;

    uint64_t bpb_phys = virt_to_phys(bpb_buf);
    if (ahci_read_sector(ahci_dev, port, 0, 1, (void*)bpb_phys) != 0) {
        kfree(bpb_buf);
        return -1;
    }

    struct bpb *bpb = (struct bpb *)bpb_buf;
    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    size_t disk_size = total_sectors * bpb->bytes_per_sector;

    // 2. Allocate a RAM buffer big enough to hold the whole disk
    uint8_t *ram_disk = kmalloc(disk_size);
    if (!ram_disk) {
        kfree(bpb_buf);
        return -1;
    }

    // 3. Read the whole disk into RAM, 64 sectors (32KB) at a time
    uint32_t sectors_read = 0;
    while (sectors_read < total_sectors) {
        uint32_t chunk = 64;
        if (sectors_read + chunk > total_sectors) {
            chunk = total_sectors - sectors_read;
        }

        uint64_t buf_phys = virt_to_phys(ram_disk + (sectors_read * bpb->bytes_per_sector));
        if (ahci_read_sector(ahci_dev, port, sectors_read, chunk, (void*)buf_phys) != 0) {
            kfree(ram_disk);
            kfree(bpb_buf);
            return -1;
        }
        sectors_read += chunk;
    }

    // 4. Initialize your FAT driver with the RAM disk!
    int ret = fat_init(ram_disk, disk_size);

    kfree(bpb_buf);
    return ret;
}

void fat_flush(void) {
    if (!fs_image || !g_ahci_dev) return;

    uint32_t total_sectors = fs_image_size / bytes_per_sec;
    uint32_t sectors_written = 0;

    while (sectors_written < total_sectors) {
        uint32_t chunk = 64; // Write 32KB at a time
        if (sectors_written + chunk > total_sectors) {
            chunk = total_sectors - sectors_written;
        }

        uint64_t buf_phys = virt_to_phys(fs_image + (sectors_written * bytes_per_sec));

        if (ahci_write_sector(g_ahci_dev, 0, sectors_written, chunk, (void*)buf_phys) != 0) {
            log_write("FAT: Flush failed!", KERNEL, LOG_ERROR);
            return;
        }
        sectors_written += chunk;
    }
    log_write("FAT: Disk flushed to AHCI.", KERNEL, LOG_INFO);
}

int fat_read_sector(uint32_t lba, void *buf) {
    if (!g_ahci_dev) return -1;

    // Allocate a physical buffer for DMA
    void *dma_buf = kmalloc(512);
    uint64_t dma_phys = virt_to_phys(dma_buf);

    // Read from AHCI Port 0 (where QEMU attached your disk.img)
    if (ahci_read_sector(g_ahci_dev, 0, lba, 1, (void*)dma_phys) != 0) {
        kfree(dma_buf);
        return -1;
    }

    // Copy the data from the DMA buffer into the FAT driver's buffer
    memcpy(buf, dma_buf, 512);
    kfree(dma_buf);
    return 0;
}
