/* Sector I/O shared by filesystem and storage transports. Buffers are virtual
 * addresses; the transport owns DMA mapping. A device may describe a partition
 * by translating its relative LBAs in the callbacks. All calls are synchronous. */
#ifndef STORAGE_BLOCK_H
#define STORAGE_BLOCK_H
#include <stddef.h>
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512u
struct block_device {
    void *context;
    uint64_t sectors;
    int (*read)(void *context, uint64_t lba, uint32_t count, void *buffer);
    int (*write)(void *context, uint64_t lba, uint32_t count, const void *buffer);
    int (*flush)(void *context);
};

static inline int block_range_valid(const struct block_device *dev,
                                    uint64_t lba, uint32_t count) {
    return dev && count && lba < dev->sectors && count <= dev->sectors - lba;
}
static inline int block_read(const struct block_device *dev, uint64_t lba,
                              uint32_t count, void *buffer) {
    return buffer && block_range_valid(dev, lba, count) && dev->read
        ? dev->read(dev->context, lba, count, buffer) : -1;
}
static inline int block_write(const struct block_device *dev, uint64_t lba,
                               uint32_t count, const void *buffer) {
    return buffer && block_range_valid(dev, lba, count) && dev->write
        ? dev->write(dev->context, lba, count, buffer) : -1;
}
static inline int block_flush(const struct block_device *dev) {
    return dev && dev->flush ? dev->flush(dev->context) : -1;
}
#endif
