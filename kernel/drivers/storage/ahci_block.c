/* A low physical page works with 32-bit HBAs and arbitrary caller buffers. */
#include "ahci_block.h"
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <utilities/string.h>

#define DMA_PAGE_SIZE 4096u

static int transfer(struct ahci_block_device *disk, uint64_t lba, uint32_t count,
                     void *buffer, int writing) {
    unsigned char *bytes = buffer;
    int result = 0;
    spin_lock(&disk->lock);
    uint64_t physical = disk->bounce_phys;
    if (!physical) result = -1;
    while (!result && count) {
        uint32_t chunk = count < DMA_PAGE_SIZE / BLOCK_SECTOR_SIZE
            ? count : DMA_PAGE_SIZE / BLOCK_SECTOR_SIZE;
        size_t length = chunk * BLOCK_SECTOR_SIZE;
        if (writing) memcpy(disk->bounce, bytes, length);
        result = writing
            ? ahci_write_sector(disk->controller, disk->port, lba, chunk, (void *)physical)
            : ahci_read_sector(disk->controller, disk->port, lba, chunk, (void *)physical);
        if (result) break;
        if (!writing) memcpy(bytes, disk->bounce, length);
        bytes += length; lba += chunk; count -= chunk;
    }
    spin_unlock(&disk->lock);
    return result;
}
static int read_sectors(void *context, uint64_t lba, uint32_t count, void *buffer) {
    return transfer(context, lba, count, buffer, 0);
}
static int write_sectors(void *context, uint64_t lba, uint32_t count, const void *buffer) {
    return transfer(context, lba, count, (void *)buffer, 1);
}
static int flush(void *context) {
    struct ahci_block_device *disk = context;
    return ahci_flush_cache(disk->controller, disk->port);
}

int ahci_block_open(struct ahci_block_device *disk, struct AHCI_DEVICE_DATA *controller, int port) {
    if (!disk || !controller || port < 0 || port >= AHCI_MAX_PORTS ||
        !controller->ports[port].is_active || controller->ports[port].signature != AHCI_SIG_SATA)
        return -1;
    memset(disk, 0, sizeof(*disk));
    disk->controller = controller;
    disk->port = port;
    disk->bounce_phys = pmm_alloc_frame_below(1ull << 32);
    if (!disk->bounce_phys) return -1;
    disk->bounce = phys_to_virt(disk->bounce_phys);
    uint64_t physical = disk->bounce_phys;
    if (!physical || ahci_identify(controller, port, (void *)physical)) goto fail;
    const uint16_t *words = (const uint16_t *)disk->bounce;
    /* This driver uses LBA48 and 512-byte logical sectors only. */
    if (!(words[83] & (1u << 10))) goto fail;
    if ((words[106] & 0xc000u) == 0x4000u && (words[106] & (1u << 12)) &&
        ((uint32_t)words[117] | ((uint32_t)words[118] << 16)) != 256) goto fail;
    uint64_t sectors = (uint64_t)words[100] | ((uint64_t)words[101] << 16) |
        ((uint64_t)words[102] << 32) | ((uint64_t)words[103] << 48);
    if (!sectors || sectors > (1ull << 48)) goto fail;
    disk->device = (struct block_device){ .context = disk, .sectors = sectors,
        .read = read_sectors, .write = write_sectors, .flush = flush };
    return 0;
fail:
    ahci_block_close(disk);
    return -1;
}
void ahci_block_close(struct ahci_block_device *disk) {
    if (!disk) return;
    if (disk->bounce_phys) pmm_free_frame(disk->bounce_phys);
    memset(disk, 0, sizeof(*disk));
}
