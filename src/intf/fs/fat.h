#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stddef.h>

#define FAT_NAME_LEN 11

/* opaque dir entry pointer (real type defined in fat.c) */
struct dir_entry;

struct fat_file {
    uint32_t first_cluster;
    uint32_t cur_cluster;
    uint32_t size;
    uint32_t pos;
    struct dir_entry *dir_ent;     /* pointer in fs_image; NULL if read-only/unlinked */
};

int    fat_init(uint8_t *image, size_t size);
int    fat_open(const char *name, struct fat_file *f);
int    fat_create(const char *name, struct fat_file *f);   /* new file in root */
size_t fat_read(struct fat_file *f, void *buf, size_t len);
size_t fat_write(struct fat_file *f, const void *buf, size_t len);
int    fat_seek(struct fat_file *f, uint32_t pos);
int    fat_unlink(const char *name);
long   fat_read_root_dir(uint32_t *index, char *buf, size_t len);

#endif
