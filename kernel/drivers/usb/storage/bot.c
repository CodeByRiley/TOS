/* USB Mass Storage Bulk-Only Transport: command, optional data, status. */
#include "usb_storage.h"
#include <utilities/string.h>

struct bot_cbw {
    uint32_t signature, tag, length;
    uint8_t flags, lun, cdb_size, cdb[16];
} __attribute__((packed));
struct bot_csw {
    uint32_t signature, tag, residue;
    uint8_t status;
} __attribute__((packed));
_Static_assert(sizeof(struct bot_cbw) == 31, "BOT command wire size");
_Static_assert(sizeof(struct bot_csw) == 13, "BOT status wire size");

static int clear_halt(struct usb_storage *disk, uint8_t endpoint) {
    struct usb_setup_packet setup = {
        .bmRequestType = 0x02, .bRequest = 1, .wIndex = endpoint,
    };
    if (disk->transport.control(disk->transport.context, &setup, 0) != 0)
        return -1;
    if (endpoint == disk->in) disk->in_toggle = 0;
    else disk->out_toggle = 0;
    return 0;
}

static void reset_recovery(struct usb_storage *disk) {
    struct usb_setup_packet setup = {
        .bmRequestType = 0x21, .bRequest = 0xff, .wIndex = disk->interface,
    };
    if (disk->transport.control(disk->transport.context, &setup, 0) != 0 ||
        clear_halt(disk, disk->in) || clear_halt(disk, disk->out))
        disk->offline = 1;
}

static int bulk(struct usb_storage *disk, int input, void *buffer,
                uint32_t length, uint32_t *actual) {
    *actual = 0;
    return disk->transport.bulk(disk->transport.context,
        input ? disk->in : disk->out,
        input ? disk->in_packet : disk->out_packet,
        input ? &disk->in_toggle : &disk->out_toggle, buffer, length, actual);
}

int usb_bot_command(struct usb_storage *disk, const void *cdb, uint8_t cdb_size,
                    void *buffer, uint32_t length, int input, uint32_t *actual) {
    if (actual) *actual = 0;
    if (!disk || !cdb || !cdb_size || cdb_size > 16 || (length && !buffer))
        return -1;
    spin_lock(&disk->lock);
    int result = -1;
    if (disk->offline) goto out;
    struct bot_cbw cbw = {
        .signature = 0x43425355, .tag = ++disk->tag,
        .length = length, .flags = input ? 0x80 : 0, .cdb_size = cdb_size,
    };
    memcpy(cbw.cdb, cdb, cdb_size);
    uint32_t transferred;
    if (bulk(disk, 0, &cbw, sizeof(cbw), &transferred) ||
        transferred != sizeof(cbw)) goto recover;

    uint32_t data_bytes = 0;
    if (length) {
        int rc = bulk(disk, input, buffer, length, &data_bytes);
        if (rc == USB_STORAGE_STALL) {
            if (clear_halt(disk, input ? disk->in : disk->out)) goto recover;
        } else if (rc) goto recover;
        if (data_bytes > length) goto recover;
    }

    struct bot_csw csw;
    int rc = bulk(disk, 1, &csw, sizeof(csw), &transferred);
    if (rc == USB_STORAGE_STALL) {
        if (clear_halt(disk, disk->in)) goto recover;
        rc = bulk(disk, 1, &csw, sizeof(csw), &transferred);
    }
    if (rc || transferred != sizeof(csw) || csw.signature != 0x53425355 ||
        csw.tag != cbw.tag || csw.residue > length || csw.status > 1)
        goto recover;
    /* Successful commands must account for all requested bytes. SCSI callers
     * can additionally require a full response rather than accepting a short. */
    if (!csw.status && data_bytes != length - csw.residue) goto recover;
    if (actual) *actual = data_bytes;
    result = csw.status;
    goto out;
recover:
    reset_recovery(disk);
out:
    spin_unlock(&disk->lock);
    return result;
}
