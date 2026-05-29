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
static uint8_t   num_fats       = 0;
static uint16_t  sectors_per_fat = 0;
static uint32_t  fat_start_sec  = 0;
static uint32_t  root_start_sec = 0;
static uint32_t  data_start_sec = 0;
static uint16_t  root_entries   = 0;
static uint16_t *fat_table      = 0;
static uint32_t  total_clusters = 0;

static uint8_t *sector(uint32_t lba) {
    return fs_image + (uint64_t)lba * bytes_per_sec;
}

/* Mirror writes to FAT2 (and any further FATs) so chkdsk doesn't complain. */
static void fat_sync_mirrors(uint32_t cluster) {
    if (num_fats < 2) return;
    for (uint8_t f = 1; f < num_fats; f++) {
        uint16_t *mirror = (uint16_t*)sector(fat_start_sec + f * sectors_per_fat);
        mirror[cluster] = fat_table[cluster];
    }
}

int fat_init(uint8_t *image, size_t size) {
    if (size < 512) return -1;
    struct bpb *b = (struct bpb*)image;
    if (b->bytes_per_sector == 0 || b->sectors_per_fat == 0) return -1;

    fs_image        = image;
    bytes_per_sec   = b->bytes_per_sector;
    sec_per_clus    = b->sectors_per_cluster;
    num_fats        = b->num_fats;
    sectors_per_fat = b->sectors_per_fat;
    fat_start_sec   = b->reserved_sectors;
    root_start_sec  = fat_start_sec + b->num_fats * b->sectors_per_fat;
    uint32_t root_sectors = (b->root_entries * 32 + bytes_per_sec - 1) / bytes_per_sec;
    data_start_sec  = root_start_sec + root_sectors;
    root_entries    = b->root_entries;
    fat_table       = (uint16_t*)sector(fat_start_sec);
    total_clusters  = (uint32_t)sectors_per_fat * bytes_per_sec / 2;

    log_write_hex("FAT: bytes/sector  =", bytes_per_sec,  KERNEL, LOG_INFO);
    log_write_hex("FAT: sec/cluster   =", sec_per_clus,   KERNEL, LOG_INFO);
    log_write_hex("FAT: fat start sec =", fat_start_sec,  KERNEL, LOG_INFO);
    log_write_hex("FAT: root start    =", root_start_sec, KERNEL, LOG_INFO);
    log_write_hex("FAT: data start    =", data_start_sec, KERNEL, LOG_INFO);
    return 0;
}

/* "readme.txt" -> "README  TXT" (8.3 padded with spaces, uppercase) */
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

static struct dir_entry *find_entry(const char *target_11) {
    struct dir_entry *root = (struct dir_entry*)sector(root_start_sec);
    for (uint16_t i = 0; i < root_entries; i++) {
        if (root[i].name[0] == 0x00) break;
        if ((uint8_t)root[i].name[0] == 0xE5) continue;
        if (root[i].attr & 0x08) continue;             /* volume label */
        if (root[i].attr & 0x10) continue;             /* subdir */
        if (memcmp(root[i].name, target_11, FAT_NAME_LEN) == 0) {
            return &root[i];
        }
    }
    return 0;
}

int fat_open(const char *name, struct fat_file *f) {
    char target[FAT_NAME_LEN];
    format_name(name, target);

    struct dir_entry *de = find_entry(target);
    if (!de) return -1;

    f->first_cluster = de->first_cluster_low;
    f->cur_cluster   = f->first_cluster;
    f->size          = de->size;
    f->pos           = 0;
    f->dir_ent       = de;
    return 0;
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
            if (next >= 0xFFF8) break;
            if (next == 0 || next == 0xFFF7) break;
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

/* ---------- WRITE SUPPORT ---------- */

/* Find first free cluster, mark as end-of-chain, return cluster number (0 = OOM). */
static uint16_t alloc_cluster(void) {
    /* clusters 0 and 1 reserved */
    for (uint32_t i = 2; i < total_clusters; i++) {
        if (fat_table[i] == 0x0000) {
            fat_table[i] = 0xFFFF;
            fat_sync_mirrors(i);
            /* zero out the cluster's data sectors */
            uint32_t cluster_sec = data_start_sec + (i - 2) * sec_per_clus;
            memset(sector(cluster_sec), 0, (uint32_t)sec_per_clus * bytes_per_sec);
            return (uint16_t)i;
        }
    }
    return 0;
}

/* Free entire FAT chain starting at first_cluster. */
static void free_chain(uint16_t first) {
    uint16_t cur = first;
    while (cur >= 2 && cur < 0xFFF8) {
        uint16_t next = fat_table[cur];
        fat_table[cur] = 0x0000;
        fat_sync_mirrors(cur);
        if (next < 2 || next == 0xFFF7) break;
        cur = next;
    }
}

/* Find empty/deleted dir entry slot. */
static struct dir_entry *alloc_dir_entry(void) {
    struct dir_entry *root = (struct dir_entry*)sector(root_start_sec);
    for (uint16_t i = 0; i < root_entries; i++) {
        if (root[i].name[0] == 0x00 || (uint8_t)root[i].name[0] == 0xE5) {
            return &root[i];
        }
    }
    return 0;
}

int fat_create(const char *name, struct fat_file *f) {
    char target[FAT_NAME_LEN];
    format_name(name, target);

    /* refuse if already exists */
    if (find_entry(target)) return -1;

    struct dir_entry *de = alloc_dir_entry();
    if (!de) return -1;

    memset(de, 0, sizeof(*de));
    memcpy(de->name, target, 8);
    memcpy(de->ext, target + 8, 3);
    de->attr              = 0x20;     /* archive */
    de->first_cluster_low = 0;
    de->size              = 0;

    f->first_cluster = 0;
    f->cur_cluster   = 0;
    f->size          = 0;
    f->pos           = 0;
    f->dir_ent       = de;
    return 0;
}

size_t fat_write(struct fat_file *f, const void *buf, size_t len) {
    if (!f->dir_ent) return 0;          /* read-only handle */

    const uint8_t *in = (const uint8_t*)buf;
    size_t total = 0;
    uint32_t cluster_bytes = (uint32_t)sec_per_clus * bytes_per_sec;

    /* if file has no clusters yet, allocate first */
    if (f->first_cluster == 0) {
        uint16_t c = alloc_cluster();
        if (c == 0) return 0;
        f->first_cluster = c;
        f->cur_cluster   = c;
        f->dir_ent->first_cluster_low = c;
    }
    if (f->cur_cluster == 0) f->cur_cluster = f->first_cluster;

    while (len > 0) {
        uint32_t off              = f->pos % cluster_bytes;
        uint32_t left_in_cluster  = cluster_bytes - off;
        size_t   chunk            = len < left_in_cluster ? len : left_in_cluster;

        uint32_t cluster_sec = data_start_sec + (f->cur_cluster - 2) * sec_per_clus;
        memcpy(sector(cluster_sec) + off, in, chunk);

        in     += chunk;
        f->pos += chunk;
        len    -= chunk;
        total  += chunk;

        /* if we hit cluster boundary and still have data, advance / allocate */
        if ((f->pos % cluster_bytes) == 0 && len > 0) {
            uint16_t next = fat_table[f->cur_cluster];
            if (next >= 0xFFF8 || next == 0 || next == 0xFFF7) {
                uint16_t c = alloc_cluster();
                if (c == 0) break;
                fat_table[f->cur_cluster] = c;
                fat_sync_mirrors(f->cur_cluster);
                f->cur_cluster = c;
            } else {
                f->cur_cluster = next;
            }
        }
    }

    if (f->pos > f->size) {
        f->size = f->pos;
        f->dir_ent->size = f->size;
    }
    return total;
}

int fat_unlink(const char *name) {
    char target[FAT_NAME_LEN];
    format_name(name, target);

    struct dir_entry *de = find_entry(target);
    if (!de) return -1;

    if (de->first_cluster_low) free_chain(de->first_cluster_low);
    de->name[0] = (char)0xE5;            /* mark deleted */
    return 0;
}

/* ---------- ENUMERATION ---------- */

static size_t dir_name_to_cstr(const struct dir_entry *entry, char *out) {
    size_t len = 0;
    for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
        out[len++] = entry->name[i];
    }
    int has_ext = 0;
    for (int i = 0; i < 3; i++) {
        if (entry->ext[i] != ' ') { has_ext = 1; break; }
    }
    if (has_ext) {
        out[len++] = '.';
        for (int i = 0; i < 3 && entry->ext[i] != ' '; i++) {
            out[len++] = entry->ext[i];
        }
    }
    out[len] = 0;
    return len;
}

long fat_read_root_dir(uint32_t *index, char *buf, size_t len) {
    if (!index || !buf) return -1;

    struct dir_entry *root = (struct dir_entry*)sector(root_start_sec);
    uint32_t i = *index;
    size_t written = 0;

    while (i < root_entries) {
        if (root[i].name[0] == 0x00) break;

        if ((uint8_t)root[i].name[0] == 0xE5 ||
            (root[i].attr & 0x08) ||
            (root[i].attr & 0x10)) {
            i++;
            continue;
        }

        char name[13];
        size_t name_len = dir_name_to_cstr(&root[i], name);
        size_t entry_len = name_len + 1;
        if (entry_len > len - written) break;

        memcpy(buf + written, name, entry_len);
        written += entry_len;
        i++;
    }

    *index = i;
    return (long)written;
}
