/* The disk command set is independent of USB host-controller mechanics. */
#include "usb_storage.h"

static uint32_t read_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}
static void write_be32(uint8_t *p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16;
    p[2] = value >> 8; p[3] = value;
}
static int command(struct usb_storage *disk, const uint8_t *cdb, uint8_t size,
                   void *buffer, uint32_t length, int input) {
    uint32_t actual;
    int rc = usb_bot_command(disk, cdb, size, buffer, length, input, &actual);
    return rc ? rc : (actual == length ? 0 : -1);
}

static int transfer(void *context, uint64_t lba, uint32_t count,
                    void *buffer, int writing) {
    struct usb_storage *disk = context;
    if (!block_range_valid(&disk->block, lba, count) || !buffer) return -1;
    uint8_t *bytes = buffer;
    while (count) {
        uint16_t chunk = count > 128 ? 128 : (uint16_t)count;
        uint8_t cdb[10] = {writing ? 0x2a : 0x28};
        write_be32(cdb + 2, (uint32_t)lba);
        cdb[7] = chunk >> 8; cdb[8] = chunk;
        if (command(disk, cdb, sizeof(cdb), bytes,
                    chunk * BLOCK_SECTOR_SIZE, !writing)) return -1;
        lba += chunk; count -= chunk; bytes += chunk * BLOCK_SECTOR_SIZE;
    }
    return 0;
}
static int read_sectors(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    return transfer(ctx, lba, count, buf, 0);
}
static int write_sectors(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    return transfer(ctx, lba, count, (void *)buf, 1);
}
static int flush(void *ctx) {
    const uint8_t cdb[10] = {0x35}; /* SYNCHRONIZE CACHE, wait for completion */
    return command(ctx, cdb, sizeof(cdb), 0, 0, 0);
}

int usb_scsi_init(struct usb_storage *disk) {
    const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t identity[36];
    if (command(disk, inquiry, sizeof(inquiry), identity, sizeof(identity), 1) ||
        (identity[0] & 0xe0) || (identity[0] & 0x1f) != 0) return -1;
    const uint8_t ready[6] = {0};
    int rc = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        rc = command(disk, ready, sizeof(ready), 0, 0, 0);
        if (rc <= 0) break;
        const uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
        uint8_t response[18];
        if (command(disk, sense, sizeof(sense), response, sizeof(response), 1) ||
            (response[2] & 0xf) != 6) return -1; /* only retry unit attention */
    }
    if (rc) return -1;
    const uint8_t capacity[10] = {0x25};
    uint8_t response[8];
    if (command(disk, capacity, sizeof(capacity), response, sizeof(response), 1))
        return -1;
    uint32_t last_lba = read_be32(response);
    if (last_lba == UINT32_MAX || read_be32(response + 4) != BLOCK_SECTOR_SIZE)
        return -1; /* READ CAPACITY(16) and non-512 sectors not implemented. */
    disk->block = (struct block_device){
        .context = disk, .sectors = (uint64_t)last_lba + 1,
        .read = read_sectors, .write = write_sectors, .flush = flush,
    };
    return 0;
}
