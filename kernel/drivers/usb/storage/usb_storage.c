/* Bind one SCSI/BOT interface and LUN 0 per configured USB device. */
#include "usb_storage.h"
#include <utilities/string.h>

static struct usb_storage disks[USB_STORAGE_MAX_DEVICES];
static size_t disk_count;

static int find_endpoints(struct usb_storage *disk, const uint8_t *config,
                          size_t length) {
    if (length < 9 || config[0] < 9 || config[1] != 2) return -1;
    size_t total = config[2] | (size_t)config[3] << 8;
    if (total > length || total < config[0]) return -1;
    int match = 0;
    for (size_t offset = config[0]; offset + 2 <= total;) {
        const uint8_t *d = config + offset;
        if (d[0] < 2 || d[0] > total - offset) return -1;
        if (d[1] == 4) {
            if (disk->in && disk->out) return 0;
            if (d[0] < 9) return -1;
            match = d[3] == 0 && d[5] == 8 && d[6] == 6 && d[7] == 0x50;
            disk->interface = d[2]; disk->in = disk->out = 0;
        } else if (match && d[1] == 5) {
            if (d[0] < 7) return -1;
            uint16_t packet = d[4] | (uint16_t)d[5] << 8;
            if ((d[3] & 3) == 2 && (d[2] & 0x0f) && !(d[2] & 0x70)) {
                if (packet != 512) return -1; /* high-speed bulk only */
                if (d[2] & 0x80) {
                    if (disk->in) return -1;
                    disk->in = d[2]; disk->in_packet = packet;
                } else {
                    if (disk->out) return -1;
                    disk->out = d[2]; disk->out_packet = packet;
                }
            }
        }
        offset += d[0];
    }
    return disk->in && disk->out ? 0 : -1;
}

int usb_storage_probe(const struct usb_storage_transport *transport,
                      const void *config, size_t length) {
    if (!transport || !transport->control || !transport->bulk || !config ||
        disk_count == USB_STORAGE_MAX_DEVICES) return -1;
    struct usb_storage *disk = &disks[disk_count];
    memset(disk, 0, sizeof(*disk));
    disk->transport = *transport;
    if (find_endpoints(disk, config, length)) return -1;
    struct usb_setup_packet setup = {
        .bmRequestType = 0xa1, .bRequest = 0xfe,
        .wIndex = disk->interface, .wLength = 1,
    };
    uint8_t max_lun = 0;
    int rc = transport->control(transport->context, &setup, &max_lun);
    if ((rc != 1 && rc != USB_STORAGE_STALL) || max_lun > 15) return -1;
    if (usb_scsi_init(disk)) return -1;
    disk_count++;
    return 0;
}

size_t usb_storage_count(void) { return disk_count; }
const struct block_device *usb_storage_device(size_t index) {
    return index < disk_count ? &disks[index].block : 0;
}
