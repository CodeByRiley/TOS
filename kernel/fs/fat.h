/* kernel/fs/fat.h , minimal FAT filesystem driver.
 *
 * Read/write FAT16 and FAT32 over an in-memory image (loaded from the GRUB
 * module payload). The on-disk type is detected from the cluster count at
 * mount time. Paths are case-insensitive; VFAT long names are read and
 * written, with 8.3 short names kept as aliases so the volume stays
 * readable by anything that only speaks 8.3.
 *
 * Nothing here knows what the image was loaded from. Backing it with a real
 * disk is kernel/fs/fat_ahci.c's job, which plugs into the write-through
 * hook below; that separation is what lets the host tests link fat.c
 * against nothing but libc.
 *
 * Implementation: kernel/fs/fat.c.
 */
#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stddef.h>

/* Bytes in a raw 8.3 directory entry name field (8 base + 3 extension). */
#define FAT_NAME_LEN 11

/* Longest VFAT long name, in characters. 20 LFN slots x 13 chars = 260,
 * but the spec caps a name at 255. */
#define FAT_LFN_MAX 255

/* Longest path accepted by the driver, including separators. */
#define FAT_PATH_MAX 260

/* Upper bound on the buffer fat_read_dir needs for one entry: a full-length
 * name, an optional trailing '/', and the NUL. */
#define FAT_DIRENT_MAX (FAT_LFN_MAX + 2)

/* Opaque on-disk dir entry , real layout in fat.c. */
struct dir_entry;

/* Open-file handle. `pos` is the byte cursor; `cur_cluster` is the
 * cluster currently mapped to it (advanced by seek/read/write). */
struct fat_file {
    uint32_t first_cluster;
    uint32_t cur_cluster;
    uint32_t size;
    uint32_t pos;
    struct dir_entry *dir_ent;     /* pointer in fs_image; NULL if read-only/unlinked */
};

/* Metadata for one path, without opening it. `attr` is the raw FAT
 * attribute byte; is_dir is broken out because that is the only bit every
 * caller wants. Directories report size 0 , FAT does not store a size for
 * them, and walking the cluster chain to synthesise one would be a lie
 * with extra steps. */
struct fat_stat {
    uint32_t size;
    uint32_t first_cluster;
    uint8_t  attr;
    uint8_t  is_dir;
};

/* Bind the driver to an in-memory FAT16 or FAT32 image. */
int    fat_init(uint8_t *image, size_t size);

/* 16 or 32 , which flavour fat_init detected. 0 before a successful mount. */
int    fat_type_bits(void);

/* Stat a root-relative path. Returns 0 on success, -1 if not found. */
int    fat_stat(const char *path, struct fat_stat *out);

/* Open an existing file by root-relative path. */
int    fat_open(const char *path, struct fat_file *f);

int    fat_create(const char *path, struct fat_file *f);

/* Discard an open file's contents, keeping its directory entry and this
 * handle valid. Allocates nothing, so it cannot fail on a good handle ,
 * which is what makes it safe where unlink-then-create is not. */
int    fat_truncate(struct fat_file *f);

size_t fat_read(struct fat_file *f, void *buf, size_t len);
size_t fat_write(struct fat_file *f, const void *buf, size_t len);
int    fat_seek(struct fat_file *f, uint32_t pos);
int    fat_unlink(const char *path);
int    fat_mkdir(const char *path);
int    fat_rmdir(const char *path);

void   fat_set_timestamp(struct dir_entry *entry);

/* Enumerate packed NUL-terminated names. Directories end in '/'. Names are
 * the VFAT long name when the entry has one, else the 8.3 name. */
long   fat_read_dir(const char *path, uint32_t *index, char *buf, size_t len);
long   fat_read_root_dir(uint32_t *index, char *buf, size_t len);
long   fat_read_dir_one(const char *path, uint32_t *index, char *buf,
                        size_t len, int *is_dir);

/* Write-through hook. The driver mutates the RAM image and then hands the
 * touched sector to `writer`, which is what pushes it at whatever the image
 * came from. `sector_data` points into the image, so the writer must not
 * hold it past the call. */
typedef void (*fat_sector_writer)(uint32_t lba, void *sector_data);

/* Install the write-through backend, or NULL to drop back to RAM-only ,
 * which is the state a host test runs in, and the reason fat.c never names
 * a storage driver itself. */
void   fat_set_sector_writer(fat_sector_writer writer);

/* The mounted image. Returns NULL when nothing is mounted; either out
 * parameter may be NULL. Exposed for the storage backend, which writes the
 * whole image back. */
uint8_t *fat_image_base(size_t *size_out, uint32_t *bytes_per_sector_out);

/* Byte size of the volume a raw boot sector describes, or 0 if its geometry
 * is unusable. A backend needs this to know how much to read before there
 * is an image to mount, and asking here keeps the BPB layout in one file.
 * `bytes_per_sector_out` may be NULL. */
size_t fat_volume_size(const void *boot_sector, uint32_t *bytes_per_sector_out);
#endif
