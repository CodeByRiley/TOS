#include <firmware/firmware.h>
#include <fs/stdio.h>
#include <memory/heap.h>
#include <stddef.h>
#include <stdint.h>

/* Firmware is copied out of the FAT image so its lifetime and alignment are
 * independent of the filesystem.  This also handles fragmented FAT files. */
#define FIRMWARE_MAX_SIZE (128ULL * 1024ULL * 1024ULL)

int firmware_load(const char *path, const void **data, uint64_t *size) {
    if (!path || !data || !size)
        return -1;

    *data = 0;
    *size = 0;

    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    long end = ftell(file);
    if (end <= 0 || (uint64_t)end > FIRMWARE_MAX_SIZE
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    void *buffer = kmalloc((size_t)end);
    if (!buffer) {
        fclose(file);
        return -1;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)end, file);
    fclose(file);
    if (bytes_read != (size_t)end) {
        kfree(buffer);
        return -1;
    }

    *data = buffer;
    *size = (uint64_t)end;
    return 0;
}

void firmware_release(const void *data) {
    kfree((void *)data);
}
