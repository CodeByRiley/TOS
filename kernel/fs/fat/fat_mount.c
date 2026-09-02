/* Volume geometry, image persistence and cluster allocation. */
#include "fat_internal.h"
#include <utilities/string.h>

#include <fs/probe.h>
#include <utilities/log.h>

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

struct fat_volume fat_volume;

/* Cluster helpers shared by both formats. */
u32 cluster_bytes(void) {
  return (u32)fat_volume.sectors_per_cluster * fat_volume.bytes_per_sector;
}

u8 *sector(u32 lba) {
  return fat_volume.image + (u64)lba * fat_volume.bytes_per_sector;
}

int cluster_is_valid(u32 cluster) {
  return cluster >= 2 && cluster < fat_volume.cluster_limit;
}

u32 cluster_eoc(void) {
  switch (fat_volume.type) {
    case FAT_TYPE_32:  return 0x0FFFFFFFu;
    default:           return 0xFFFFu;
  }
}

int cluster_is_eoc(u32 value) {
  switch (fat_volume.type) {
    case FAT_TYPE_32:  return value >= 0x0FFFFFF8u;
    default:           return value >= 0xFFF8u;
  }
}

u32 fat_get(u32 cluster) {
  u8 *table = sector(fat_volume.fat_start_sector);
  switch (fat_volume.type) {
    case FAT_TYPE_32:
      return ((u32 *)table)[cluster] & 0x0FFFFFFFu;
    default:
      return ((u16 *)table)[cluster];
  }
}

static void fat_put(u8 *table, u32 cluster, u32 value) {
  switch (fat_volume.type) {
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
  usize entry_width = (fat_volume.type == FAT_TYPE_16) ? 2u : 4u;
  for (u8 f = 0; f < fat_volume.fat_count; f++) {
    u8 *table = sector(fat_volume.fat_start_sector + f * fat_volume.sectors_per_fat);
    fat_put(table, cluster, value);
    fat_flush_bytes(table + (usize)cluster * entry_width, entry_width);
  }
}

int fat_type_bits(void) {
  return (int)fat_volume.type;
}

void fat_set_sector_writer(fat_sector_writer writer) {
  fat_volume.write_sector = writer;
}

int fat_write_through_enabled(void) {
  return fat_volume.write_sector != 0;
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
    *size_out = fat_volume.image ? fat_volume.image_size : 0;
  if (bytes_per_sector_out)
    *bytes_per_sector_out = fat_volume.image ? fat_volume.bytes_per_sector : 0;
  return fat_volume.image;
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
  return sector(fat_volume.data_start_sector + (cluster - 2) * fat_volume.sectors_per_cluster);
}

void fat_flush_sector(u32 lba) {
  if (fat_volume.image && fat_volume.write_sector)
    fat_volume.write_sector(lba, sector(lba));
}

void fat_flush_bytes(const void *start, usize len) {
  if (!fat_volume.image || !fat_volume.write_sector || !start || len == 0 || !fat_volume.bytes_per_sector)
    return;

  u64 base = (u64)(uintptr_t)fat_volume.image;
  u64 at = (u64)(uintptr_t)start;
  if (at < base || at >= base + fat_volume.image_size)
    return;

  u64 off = at - base;
  if (len > fat_volume.image_size - off)
    len = (usize)(fat_volume.image_size - off);

  u32 first = (u32)(off / fat_volume.bytes_per_sector);
  u32 last = (u32)((off + len - 1) / fat_volume.bytes_per_sector);
  for (u32 lba = first; lba <= last; lba++)
    fat_flush_sector(lba);
}

struct fat_dir root_directory(void) {
  struct fat_dir dir = { .is_root = 1, .first_cluster = 0 };
  if (fat_volume.type != FAT_TYPE_16)
    dir.first_cluster = fat_volume.root_cluster;
  return dir;
}

int dir_is_fixed_root(struct fat_dir dir) {
  return dir.is_root && fat_volume.type == FAT_TYPE_16;
}

static int fat_mount_format(u8 *image, usize size) {
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

  fat_volume.image = image;
  fat_volume.image_size = size;
  fat_volume.bytes_per_sector = b->bytes_per_sector;
  fat_volume.sectors_per_cluster = b->sectors_per_cluster;
  fat_volume.fat_count = b->num_fats;
  fat_volume.sectors_per_fat = fat_sectors;
  fat_volume.fat_start_sector = fat_start;
  fat_volume.root_start_sector = root_start;
  fat_volume.data_start_sector = data_start;
  fat_volume.root_entries = b->root_entries;
  fat_volume.type = type;
  fat_volume.cluster_limit = limit;
  fat_volume.root_cluster = type == FAT_TYPE_32 ? b->root_cluster : 0;

  log_write_hex("FAT: type          =", (u64)fat_volume.type, KERNEL, LOG_INFO);
  log_write_hex("FAT: bytes/sector  =", fat_volume.bytes_per_sector, KERNEL, LOG_INFO);
  log_write_hex("FAT: sec/cluster   =", fat_volume.sectors_per_cluster, KERNEL, LOG_INFO);
  log_write_hex("FAT: clusters      =", fat_volume.cluster_limit, KERNEL, LOG_INFO);
  return 0;
}

u32 fat_impl_alloc_cluster(void) {
  for (u32 c = 2; c < fat_volume.cluster_limit; c++) {
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
  while (cluster_is_valid(current) && visited++ < fat_volume.cluster_limit) {
    u32 next = fat_get(current);
    fat_set(current, 0);
    if (cluster_is_eoc(next) || !cluster_is_valid(next))
      break;
    current = next;
  }
}

int fat_init(u8 *image, usize size) {
  if (!image || size < 512)
    return -1;
  if (fs_probe_is_exfat(image, size))
    return -1;

  return fat_mount_format(image, size);
}
