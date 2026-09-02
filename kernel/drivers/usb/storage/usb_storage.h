#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include <devices/usb.h>
#include <drivers/storage/block.h>
#include <sync/spinlock.h>

#define USB_STORAGE_MAX_DEVICES 8
#define USB_STORAGE_STALL (-2)

/* HCD owns context. Bulk reports actual bytes, including short transfers;
 * endpoint toggles survive calls and reset only after CLEAR_FEATURE. */
struct usb_storage_transport {
    void *context;
    int (*control)(void *, const struct usb_setup_packet *, void *);
    int (*bulk)(void *, uint8_t endpoint, uint16_t packet, uint8_t *toggle,
                void *buffer, uint32_t length, uint32_t *actual);
};

struct usb_storage {
    struct usb_storage_transport transport;
    struct block_device block;
    struct spinlock lock;
    uint32_t tag;
    uint8_t interface, in, out, in_toggle, out_toggle, offline;
    uint16_t in_packet, out_packet;
};

/* BOT: 0 success, 1 SCSI command failed, -1 transport/protocol failure.
 * A failed command is never automatically replayed (especially a write). */
int usb_bot_command(struct usb_storage *, const void *cdb, uint8_t cdb_size,
                    void *buffer, uint32_t length, int input, uint32_t *actual);
int usb_scsi_init(struct usb_storage *);
int usb_storage_probe(const struct usb_storage_transport *, const void *config,
                      size_t length);
size_t usb_storage_count(void);
const struct block_device *usb_storage_device(size_t index);

#endif
