/* Minimal read/write FAT16 driver over an in-memory disk image.
 *
 * Paths are root-relative, case-insensitive FAT 8.3 names. Both '/' and
 * '\\' separate components. The fixed FAT16 root table and cluster-backed
 * subdirectories share the same lookup/enumeration path.
 */
#include "fs/fat.h"
#include "utilities/log.h"
#include "utilities/string.h"

#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F
#define FAT_CLUSTER_BAD    0xFFF7
#define FAT_CLUSTER_EOC    0xFFF8
#define FAT_PATH_MAX       256
#define FAT_MAX_COMPONENTS 16
#define FAT_COMPONENT_MAX  12

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

_Static_assert(sizeof(struct dir_entry) == 32,
               "FAT directory entries must be 32 bytes");

struct fat_dir {
    uint16_t first_cluster;
    int root;
};

struct path_parts {
    char component[FAT_MAX_COMPONENTS][FAT_COMPONENT_MAX + 1];
    uint8_t count;
};

struct dir_cursor {
    struct fat_dir dir;
    uint16_t cluster;
    uint32_t slot;
    uint32_t index;
    uint32_t clusters_seen;
    int ended;
};

static uint8_t  *fs_image;
static size_t    fs_image_size;
static uint16_t  bytes_per_sec;
static uint8_t   sec_per_clus;
static uint8_t   num_fats;
static uint16_t  sectors_per_fat;
static uint32_t  fat_start_sec;
static uint32_t  root_start_sec;
static uint32_t  data_start_sec;
static uint16_t  root_entries;
static uint16_t *fat_table;
/* Exclusive upper bound; valid data clusters are [2, cluster_limit). */
static uint32_t  cluster_limit;

static uint32_t cluster_bytes(void) {
    return (uint32_t)sec_per_clus * bytes_per_sec;
}

static uint8_t *sector(uint32_t lba) {
    return fs_image + (uint64_t)lba * bytes_per_sec;
}

static int cluster_is_valid(uint32_t cluster) {
    return cluster >= 2 && cluster < cluster_limit;
}

/* Return 0 for a valid successor, 1 for EOC, and -1 for corruption. */
static int next_cluster(uint16_t cluster, uint16_t *next) {
    if (!cluster_is_valid(cluster))
        return -1;
    uint16_t value = fat_table[cluster];
    if (value >= FAT_CLUSTER_EOC)
        return 1;
    if (!cluster_is_valid(value) || value == FAT_CLUSTER_BAD)
        return -1;
    *next = value;
    return 0;
}

static uint8_t *cluster_data(uint16_t cluster) {
    if (!cluster_is_valid(cluster))
        return 0;
    uint32_t lba = data_start_sec
                 + ((uint32_t)cluster - 2) * sec_per_clus;
    return sector(lba);
}

/* Mirror writes to every FAT copy. */
static void fat_set(uint16_t cluster, uint16_t value) {
    fat_table[cluster] = value;
    for (uint8_t f = 1; f < num_fats; f++) {
        uint16_t *mirror =
            (uint16_t *)sector(fat_start_sec + f * sectors_per_fat);
        mirror[cluster] = value;
    }
}

int fat_init(uint8_t *image, size_t size) {
    if (!image || size < 512)
        return -1;

    struct bpb *b = (struct bpb *)image;
    uint32_t total_sectors = b->total_sectors_16
                           ? b->total_sectors_16 : b->total_sectors_32;
    if (b->bytes_per_sector < 512 || b->bytes_per_sector > 4096
        || (b->bytes_per_sector & (b->bytes_per_sector - 1)) != 0
        || b->sectors_per_cluster == 0 || b->reserved_sectors == 0
        || b->num_fats == 0 || b->sectors_per_fat == 0
        || total_sectors == 0
        || (uint64_t)total_sectors * b->bytes_per_sector > size)
        return -1;

    uint32_t root_sectors =
        ((uint32_t)b->root_entries * sizeof(struct dir_entry)
         + b->bytes_per_sector - 1) / b->bytes_per_sector;
    uint32_t fat_start = b->reserved_sectors;
    uint32_t root_start = fat_start
                        + (uint32_t)b->num_fats * b->sectors_per_fat;
    uint32_t data_start = root_start + root_sectors;
    if (root_start < fat_start || data_start < root_start
        || data_start >= total_sectors)
        return -1;

    uint32_t data_clusters =
        (total_sectors - data_start) / b->sectors_per_cluster;
    uint32_t fat_entries =
        (uint32_t)b->sectors_per_fat * b->bytes_per_sector / 2;
    uint32_t limit = data_clusters + 2;
    if (limit > fat_entries)
        limit = fat_entries;
    if (limit <= 2 || limit >= 0xFFF0)
        return -1;

    fs_image        = image;
    fs_image_size   = size;
    bytes_per_sec   = b->bytes_per_sector;
    sec_per_clus    = b->sectors_per_cluster;
    num_fats        = b->num_fats;
    sectors_per_fat = b->sectors_per_fat;
    fat_start_sec   = fat_start;
    root_start_sec  = root_start;
    data_start_sec  = data_start;
    root_entries    = b->root_entries;
    fat_table       = (uint16_t *)sector(fat_start_sec);
    cluster_limit   = limit;

    (void)fs_image_size;
    log_write_hex("FAT: bytes/sector  =", bytes_per_sec, KERNEL, LOG_INFO);
    log_write_hex("FAT: sec/cluster   =", sec_per_clus, KERNEL, LOG_INFO);
    log_write_hex("FAT: fat start sec =", fat_start_sec, KERNEL, LOG_INFO);
    log_write_hex("FAT: root start    =", root_start_sec, KERNEL, LOG_INFO);
    log_write_hex("FAT: data start    =", data_start_sec, KERNEL, LOG_INFO);
    return 0;
}

static int is_separator(char c) {
    return c == '/' || c == '\\';
}

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
        while (pos < FAT_PATH_MAX && path[pos] != '\0'
               && !is_separator(path[pos]))
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
        if (length == 0 || length > FAT_COMPONENT_MAX
            || parts->count >= FAT_MAX_COMPONENTS)
            return -1;

        char *out = parts->component[parts->count++];
        for (uint32_t i = 0; i < length; i++)
            out[i] = path[start + i];
        out[length] = '\0';
    }
}

static int valid_short_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9'))
        return 1;
    switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~':
    case '`': case '!': case '(': case ')': case '{': case '}': case '^':
    case '#': case '&':
        return 1;
    default:
        return 0;
    }
}

static int format_component(const char *component, char out[FAT_NAME_LEN]) {
    if (!component || !component[0])
        return -1;
    memset(out, ' ', FAT_NAME_LEN);

    uint32_t base = 0;
    uint32_t ext = 0;
    int in_ext = 0;
    for (uint32_t i = 0; component[i]; i++) {
        char c = component[i];
        if (c == '.') {
            if (in_ext || base == 0)
                return -1;
            in_ext = 1;
            continue;
        }
        if (!valid_short_char(c))
            return -1;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        if (!in_ext) {
            if (base >= 8)
                return -1;
            out[base++] = c;
        } else {
            if (ext >= 3)
                return -1;
            out[8 + ext++] = c;
        }
    }
    return base > 0 ? 0 : -1;
}

static struct fat_dir root_directory(void) {
    struct fat_dir dir = { .first_cluster = 0, .root = 1 };
    return dir;
}

static int cursor_init(struct dir_cursor *cursor, struct fat_dir dir,
                       uint32_t start_index) {
    memset(cursor, 0, sizeof(*cursor));
    cursor->dir = dir;
    cursor->index = start_index;

    if (dir.root) {
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
        uint16_t next;
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

    if (cursor->dir.root) {
        if (cursor->slot >= root_entries) {
            cursor->ended = 1;
            return 0;
        }
        struct dir_entry *root =
            (struct dir_entry *)sector(root_start_sec);
        cursor->index++;
        return &root[cursor->slot++];
    }

    uint32_t entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
    if (cursor->slot >= entries_per_cluster) {
        uint16_t next;
        int status = next_cluster(cursor->cluster, &next);
        if (status != 0 || ++cursor->clusters_seen >= cluster_limit) {
            cursor->ended = 1;
            return 0;
        }
        cursor->cluster = next;
        cursor->slot = 0;
    }

    struct dir_entry *entries =
        (struct dir_entry *)cluster_data(cursor->cluster);
    if (!entries) {
        cursor->ended = 1;
        return 0;
    }
    cursor->index++;
    return &entries[cursor->slot++];
}

static int entry_is_usable(const struct dir_entry *entry) {
    uint8_t first = (uint8_t)entry->name[0];
    return first != 0x00 && first != 0xE5
        && (entry->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN
        && !(entry->attr & FAT_ATTR_VOLUME_ID);
}

static struct dir_entry *find_in_directory(struct fat_dir dir,
                                            const char target[FAT_NAME_LEN]) {
    struct dir_cursor cursor;
    if (cursor_init(&cursor, dir, 0) != 0)
        return 0;

    struct dir_entry *entry;
    while ((entry = cursor_next(&cursor)) != 0) {
        if ((uint8_t)entry->name[0] == 0x00)
            return 0;
        if (entry_is_usable(entry)
            && memcmp(entry->name, target, FAT_NAME_LEN) == 0)
            return entry;
    }
    return 0;
}

static int walk_directories(const struct path_parts *parts, uint32_t count,
                            struct fat_dir *out) {
    struct fat_dir current = root_directory();
    for (uint32_t i = 0; i < count; i++) {
        char target[FAT_NAME_LEN];
        if (format_component(parts->component[i], target) != 0)
            return -1;
        struct dir_entry *entry = find_in_directory(current, target);
        if (!entry || !(entry->attr & FAT_ATTR_DIRECTORY)
            || !cluster_is_valid(entry->first_cluster_low))
            return -1;
        current.root = 0;
        current.first_cluster = entry->first_cluster_low;
    }
    *out = current;
    return 0;
}

static int resolve_parent(const char *path, struct fat_dir *parent,
                          char leaf[FAT_NAME_LEN]) {
    struct path_parts parts;
    if (split_path(path, &parts) != 0 || parts.count == 0
        || format_component(parts.component[parts.count - 1], leaf) != 0)
        return -1;
    return walk_directories(&parts, parts.count - 1, parent);
}

static struct dir_entry *resolve_entry(const char *path,
                                       struct fat_dir *parent_out) {
    struct fat_dir parent;
    char leaf[FAT_NAME_LEN];
    if (resolve_parent(path, &parent, leaf) != 0)
        return 0;
    if (parent_out)
        *parent_out = parent;
    return find_in_directory(parent, leaf);
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
    if (entry->size != 0 && !cluster_is_valid(entry->first_cluster_low))
        return -1;

    file->first_cluster = entry->first_cluster_low;
    file->cur_cluster = file->first_cluster;
    file->size = entry->size;
    file->pos = 0;
    file->dir_ent = entry;
    return 0;
}

size_t fat_read(struct fat_file *file, void *buffer, size_t length) {
    if (!file || !buffer)
        return 0;
    uint8_t *out = (uint8_t *)buffer;
    size_t total = 0;
    uint32_t bytes = cluster_bytes();

    while (length > 0 && file->pos < file->size) {
        uint8_t *data = cluster_data((uint16_t)file->cur_cluster);
        if (!data)
            break;
        uint32_t offset = file->pos % bytes;
        uint32_t in_cluster = bytes - offset;
        uint32_t in_file = file->size - file->pos;
        size_t chunk = length;
        if (chunk > in_cluster) chunk = in_cluster;
        if (chunk > in_file) chunk = in_file;

        memcpy(out, data + offset, chunk);
        out += chunk;
        file->pos += (uint32_t)chunk;
        length -= chunk;
        total += chunk;

        if (file->pos % bytes == 0 && file->pos < file->size) {
            uint16_t next;
            if (next_cluster((uint16_t)file->cur_cluster, &next) != 0)
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
        uint16_t next;
        if (next_cluster((uint16_t)file->cur_cluster, &next) != 0)
            return -1;
        file->cur_cluster = next;
    }
    file->pos = position;
    return 0;
}

static uint16_t alloc_cluster(void) {
    for (uint32_t cluster = 2; cluster < cluster_limit; cluster++) {
        if (fat_table[cluster] == 0) {
            fat_set((uint16_t)cluster, 0xFFFF);
            memset(cluster_data((uint16_t)cluster), 0, cluster_bytes());
            return (uint16_t)cluster;
        }
    }
    return 0;
}

static void free_chain(uint16_t first) {
    uint16_t current = first;
    uint32_t visited = 0;
    while (cluster_is_valid(current) && visited++ < cluster_limit) {
        uint16_t next = fat_table[current];
        fat_set(current, 0);
        if (next >= FAT_CLUSTER_EOC || !cluster_is_valid(next))
            break;
        current = next;
    }
}

static struct dir_entry *alloc_dir_entry(struct fat_dir dir) {
    struct dir_entry *deleted = 0;
    if (dir.root) {
        struct dir_entry *root =
            (struct dir_entry *)sector(root_start_sec);
        for (uint32_t i = 0; i < root_entries; i++) {
            uint8_t first = (uint8_t)root[i].name[0];
            if (first == 0xE5 && !deleted)
                deleted = &root[i];
            if (first == 0x00)
                return deleted ? deleted : &root[i];
        }
        return deleted;
    }

    if (!cluster_is_valid(dir.first_cluster))
        return 0;
    uint16_t current = dir.first_cluster;
    uint32_t entries_per_cluster = cluster_bytes() / sizeof(struct dir_entry);
    uint32_t visited = 0;
    while (visited++ < cluster_limit) {
        struct dir_entry *entries =
            (struct dir_entry *)cluster_data(current);
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first = (uint8_t)entries[i].name[0];
            if (first == 0xE5 && !deleted)
                deleted = &entries[i];
            if (first == 0x00)
                return deleted ? deleted : &entries[i];
        }

        uint16_t next;
        int status = next_cluster(current, &next);
        if (status == 0) {
            current = next;
            continue;
        }
        if (status < 0)
            return 0;
        if (deleted)
            return deleted;

        uint16_t new_cluster = alloc_cluster();
        if (!new_cluster)
            return 0;
        fat_set(current, new_cluster);
        return (struct dir_entry *)cluster_data(new_cluster);
    }
    return 0;
}

int fat_create(const char *path, struct fat_file *file) {
    if (!file)
        return -1;
    struct fat_dir parent;
    char leaf[FAT_NAME_LEN];
    if (resolve_parent(path, &parent, leaf) != 0
        || find_in_directory(parent, leaf))
        return -1;

    struct dir_entry *entry = alloc_dir_entry(parent);
    if (!entry)
        return -1;
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, leaf, FAT_NAME_LEN);
    entry->attr = FAT_ATTR_ARCHIVE;

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
        uint16_t cluster = alloc_cluster();
        if (!cluster)
            return 0;
        file->first_cluster = cluster;
        file->cur_cluster = cluster;
        file->dir_ent->first_cluster_low = cluster;
    }

    /* At an exact EOF boundary cur_cluster names the preceding cluster. */
    if (length > 0 && file->pos > 0 && file->pos == file->size
        && file->pos % bytes == 0) {
        uint16_t next;
        int status = next_cluster((uint16_t)file->cur_cluster, &next);
        if (status == 1) {
            next = alloc_cluster();
            if (!next)
                return 0;
            fat_set((uint16_t)file->cur_cluster, next);
        } else if (status < 0) {
            return 0;
        }
        file->cur_cluster = next;
    }

    while (length > 0) {
        uint8_t *data = cluster_data((uint16_t)file->cur_cluster);
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
            uint16_t next;
            int status = next_cluster((uint16_t)file->cur_cluster, &next);
            if (status == 1) {
                next = alloc_cluster();
                if (!next)
                    break;
                fat_set((uint16_t)file->cur_cluster, next);
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
    if (file->pos > 0 && file->pos < file->size
        && file->pos % bytes == 0) {
        uint16_t next;
        if (next_cluster((uint16_t)file->cur_cluster, &next) == 0)
            file->cur_cluster = next;
    }
    return total;
}

int fat_unlink(const char *path) {
    struct dir_entry *entry = resolve_entry(path, 0);
    if (!entry || (entry->attr & FAT_ATTR_DIRECTORY))
        return -1;
    if (entry->first_cluster_low)
        free_chain(entry->first_cluster_low);
    entry->name[0] = (char)0xE5;
    return 0;
}

static void set_dot_entry(struct dir_entry *entry, int parent,
                          uint16_t cluster) {
    memset(entry, 0, sizeof(*entry));
    memset(entry->name, ' ', FAT_NAME_LEN);
    entry->name[0] = '.';
    if (parent)
        entry->name[1] = '.';
    entry->attr = FAT_ATTR_DIRECTORY;
    entry->first_cluster_low = cluster;
}

int fat_mkdir(const char *path) {
    struct fat_dir parent;
    char leaf[FAT_NAME_LEN];
    if (resolve_parent(path, &parent, leaf) != 0
        || find_in_directory(parent, leaf))
        return -1;

    uint16_t cluster = alloc_cluster();
    if (!cluster)
        return -1;
    struct dir_entry *children =
        (struct dir_entry *)cluster_data(cluster);
    set_dot_entry(&children[0], 0, cluster);
    set_dot_entry(&children[1], 1,
                  parent.root ? 0 : parent.first_cluster);

    struct dir_entry *entry = alloc_dir_entry(parent);
    if (!entry) {
        free_chain(cluster);
        return -1;
    }
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, leaf, FAT_NAME_LEN);
    entry->attr = FAT_ATTR_DIRECTORY;
    entry->first_cluster_low = cluster;
    return 0;
}

static int is_dot_entry(const struct dir_entry *entry) {
    return entry->name[0] == '.'
        && (entry->name[1] == ' ' || entry->name[1] == '.');
}

static size_t entry_name(const struct dir_entry *entry, char *out) {
    size_t length = 0;
    for (int i = 0; i < 8 && entry->name[i] != ' '; i++)
        out[length++] = entry->name[i];

    int has_extension = 0;
    for (int i = 0; i < 3; i++) {
        if (entry->ext[i] != ' ') {
            has_extension = 1;
            break;
        }
    }
    if (has_extension) {
        out[length++] = '.';
        for (int i = 0; i < 3 && entry->ext[i] != ' '; i++)
            out[length++] = entry->ext[i];
    }
    if (entry->attr & FAT_ATTR_DIRECTORY)
        out[length++] = '/';
    out[length] = '\0';
    return length;
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
    size_t written = 0;
    struct dir_entry *entry;
    while ((entry = cursor_next(&cursor)) != 0) {
        if ((uint8_t)entry->name[0] == 0x00)
            break;
        if (!entry_is_usable(entry) || is_dot_entry(entry))
            continue;

        char name[14];
        size_t name_length = entry_name(entry, name);
        size_t required = name_length + 1;
        if (required > length - written) {
            if (written == 0)
                return -1;
            cursor.index--;
            break;
        }
        memcpy(buffer + written, name, required);
        written += required;
    }
    *index = cursor.index;
    return (long)written;
}

long fat_read_root_dir(uint32_t *index, char *buffer, size_t length) {
    return fat_read_dir("/", index, buffer, length);
}
