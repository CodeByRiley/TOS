#include "fs/fat.h"
#include "utilities/string.h"
#include "utilities/log.h"

struct __attribute__((packed)) bpb {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
};

struct __attribute__((packed)) dir_entry {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved[10];
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t size;
};

static uint8_t  *fs_image       = 0;
static uint16_t  bytes_per_sec  = 0;
static uint8_t   sec_per_clus   = 0;
static uint32_t  fat_start_sec  = 0;
static uint32_t  root_start_sec = 0;
static uint32_t  data_start_sec = 0;
static uint16_t  root_entries   = 0;
static uint16_t *fat_table      = 0;

static uint8_t *sector(uint32_t lba) {
    return fs_image + (uint64_t)lba * bytes_per_sec;
}

int fat_init(uint8_t *image, size_t size) {
    if (size < 512) return -1;
    struct bpb *b = (struct bpb*)image;
    if (b->bytes_per_sector == 0 || b->sectors_per_fat == 0) return -1;

    fs_image       = image;
    bytes_per_sec  = b->bytes_per_sector;
    sec_per_clus   = b->sectors_per_cluster;
    fat_start_sec  = b->reserved_sectors;
    root_start_sec = fat_start_sec + b->num_fats * b->sectors_per_fat;
    uint32_t root_sectors = (b->root_entries * 32 + bytes_per_sec - 1) / bytes_per_sec;
    data_start_sec = root_start_sec + root_sectors;
    root_entries   = b->root_entries;
    fat_table      = (uint16_t*)sector(fat_start_sec);

    log_write_hex("FAT: bytes/sector  =", bytes_per_sec,  KERNEL, LOG_INFO);
    log_write_hex("FAT: sec/cluster   =", sec_per_clus,   KERNEL, LOG_INFO);
    log_write_hex("FAT: fat start sec =", fat_start_sec,  KERNEL, LOG_INFO);
    log_write_hex("FAT: root start    =", root_start_sec, KERNEL, LOG_INFO);
    log_write_hex("FAT: data start    =", data_start_sec, KERNEL, LOG_INFO);
    return 0;
}

// "readme.txt" -> "README  TXT" (8.3 padded with spaces, uppercase)
static void format_name(const char *name, char out[FAT_NAME_LEN]) {
    memset(out, ' ', FAT_NAME_LEN);
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
        i++;
    }
    if (name[i] == '.') i++;
    j = 8;
    while (name[i] && j < 11) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
        i++;
    }
}

int fat_open(const char *name, struct fat_file *f) {
    char target[FAT_NAME_LEN];
    format_name(name, target);

    struct dir_entry *root = (struct dir_entry*)sector(root_start_sec);
    for (uint16_t i = 0; i < root_entries; i++) {
        if (root[i].name[0] == 0x00) break;            // end of directory
        if ((uint8_t)root[i].name[0] == 0xE5) continue; // deleted
        if (root[i].attr & 0x08) continue;             // volume label
        if (root[i].attr & 0x10) continue;             // subdir (skip for now)
        if (memcmp(root[i].name, target, FAT_NAME_LEN) == 0) {
            f->first_cluster = root[i].first_cluster_low;
            f->cur_cluster   = f->first_cluster;
            f->size          = root[i].size;
            f->pos           = 0;
            return 0;
        }
    }
    return -1;
}

size_t fat_read(struct fat_file *f, void *buf, size_t len) {
    uint8_t *out = (uint8_t*)buf;
    size_t total = 0;
    uint32_t cluster_bytes = (uint32_t)sec_per_clus * bytes_per_sec;

    while (len > 0 && f->pos < f->size) {
        uint32_t cluster_sec     = data_start_sec + (f->cur_cluster - 2) * sec_per_clus;
        uint32_t off_in_cluster  = f->pos % cluster_bytes;
        uint32_t left_in_cluster = cluster_bytes - off_in_cluster;
        uint32_t left_in_file    = f->size - f->pos;

        size_t chunk = len;
        if (chunk > left_in_cluster) chunk = left_in_cluster;
        if (chunk > left_in_file)    chunk = left_in_file;

        memcpy(out, sector(cluster_sec) + off_in_cluster, chunk);
        out    += chunk;
        f->pos += chunk;
        len    -= chunk;
        total  += chunk;

        if (f->pos % cluster_bytes == 0 && f->pos < f->size) {
            uint16_t next = fat_table[f->cur_cluster];
            if (next >= 0xFFF8) break;          // end of chain
            if (next == 0 || next == 0xFFF7) break; // free or bad
            f->cur_cluster = next;
        }
    }
    return total;
}

int fat_seek(struct fat_file *f, uint32_t pos) {
    if (pos > f->size) pos = f->size;
    uint32_t cluster_bytes = (uint32_t)sec_per_clus * bytes_per_sec;
    f->cur_cluster = f->first_cluster;
    uint32_t skip = pos / cluster_bytes;
    while (skip--) {
        uint16_t next = fat_table[f->cur_cluster];
        if (next >= 0xFFF8 || next == 0) break;
        f->cur_cluster = next;
    }
    f->pos = pos;
    return 0;
}
