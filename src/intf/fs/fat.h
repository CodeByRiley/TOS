/* src/intf/fs/fat.h — minimal FAT filesystem driver.
 *
 * Read/write FAT16 over an in-memory image (loaded from the GRUB module
 * payload). Supports root-relative 8.3 paths, cluster-backed directories,
 * create, read, write, seek, unlink, mkdir, and enumeration.
 *
 * Implementation: src/impl/kernel/fs/fat.c.
 */
#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stddef.h>

#define FAT_NAME_LEN 11

/* Opaque on-disk dir entry — real layout in fat.c. */
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

/* Bind the driver to an in-memory FAT16 image. */
int    fat_init(uint8_t *image, size_t size);

/* Open an existing file by root-relative 8.3 path. */
int    fat_open(const char *path, struct fat_file *f);

int    fat_create(const char *path, struct fat_file *f);

size_t fat_read(struct fat_file *f, void *buf, size_t len);
size_t fat_write(struct fat_file *f, const void *buf, size_t len);
int    fat_seek(struct fat_file *f, uint32_t pos);
int    fat_unlink(const char *path);
int    fat_mkdir(const char *path);

/* Enumerate packed NUL-terminated names. Directories end in '/'. */
long   fat_read_dir(const char *path, uint32_t *index, char *buf, size_t len);
long   fat_read_root_dir(uint32_t *index, char *buf, size_t len);

#endif
