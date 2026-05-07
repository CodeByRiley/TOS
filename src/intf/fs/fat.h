#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include <stddef.h>

#define FAT_NAME_LEN 11

struct fat_file {
    uint32_t first_cluster;
    uint32_t cur_cluster;
    uint32_t size;
    uint32_t pos;
};

int    fat_init(uint8_t *image, size_t size);
int    fat_open(const char *name, struct fat_file *f);
size_t fat_read(struct fat_file *f, void *buf, size_t len);
int 	 fat_seek(struct fat_file *f, uint32_t pos);

#endif
