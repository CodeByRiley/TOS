/* kernel/drivers/usb/usb_device.h , the USB device model shared by host
 * controllers.
 *
 * Everything a controller does to a device on a freshly reset port , read its
 * descriptors, give it an address, choose a configuration, find a HID pointer
 * worth polling , is the same sequence of control transfers whichever
 * controller is driving. What differs is only how a control transfer reaches
 * the wire: UHCI walks a TD chain and cares whether the device is low-speed,
 * EHCI queues qTDs against a timeout.
 *
 * So a controller supplies that one primitive and gets the rest. struct
 * usb_pipe is the seam: it names a device and carries the transfer function.
 *
 * Nothing here touches controller registers, allocates descriptors, or knows
 * what a port is. That stays in the controller.
 *
 * Implementation: kernel/drivers/usb/usb_device.c.
 */
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <devices/usb.h>
#include <utilities/types.h>

struct usb_pipe;

/* Issue one control transfer to the pipe's device. `in` is 1 for
 * device-to-host. Returns bytes transferred, or negative on failure , the
 * convention both controllers already use. */
typedef int (*usb_control_fn)(const struct usb_pipe *pipe,
                              const struct usb_setup_packet *setup, void *data,
                              u16 length, int in);

struct usb_pipe {
    /* Controller-private, handed back to `control` untouched. */
    void *hc;
    usb_control_fn control;
    u8 address;      /* 0 until SET_ADDRESS is accepted */
    u16 maxpacket;   /* endpoint 0 packet size */
    int low_speed;   /* UHCI puts this in its TDs; EHCI ignores it */
    const char *tag; /* "UHCI" / "EHCI", prefixes log lines */
};

/* Standard device requests. Each returns what the transfer returned. */
int usb_get_descriptor(const struct usb_pipe *pipe, u8 type, u8 index,
                       void *out, u16 length);
int usb_set_address(const struct usb_pipe *pipe, u8 address);
int usb_set_configuration(const struct usb_pipe *pipe, u8 configuration);

/* HID class requests. Both are advisory: a device that refuses them is still
 * usable, so callers log and continue rather than abandoning the device. */
int usb_hid_set_idle(const struct usb_pipe *pipe, u8 interface);
int usb_hid_set_boot_protocol(const struct usb_pipe *pipe, u8 interface);

/* Read a configuration block into `buffer`: the 9-byte header first for
 * wTotalLength, then the whole block. A device claiming more than `capacity`
 * is truncated rather than allowed to overrun. Returns bytes read, or -1. */
int usb_read_config(const struct usb_pipe *pipe, u8 index, u8 *buffer,
                    u16 capacity);

/* What kind of pointing device an interrupt endpoint belongs to. */
#define USB_INT_KIND_NONE 0
#define USB_INT_KIND_HID_BOOT_MOUSE 1
#define USB_INT_KIND_HID_TABLET 2

struct usb_hid_pointer {
    u8 interface_number;
    u8 kind;
    struct usb_endpoint_descriptor endpoint;
};

/* Walk a configuration block for the first interrupt-IN endpoint belonging to
 * a HID pointing device, and fill `out`. Returns 1 when one was found.
 *
 * `max_packet_limit` rejects an endpoint whose packets would not fit the
 * caller's interrupt buffer; pass 0 for no limit. The two controllers differ
 * here , EHCI has a fixed interrupt buffer and UHCI does not , which is why it
 * is an argument rather than a constant.
 *
 * Matching is deliberately narrow. A boot-protocol mouse is recognised from
 * its interface descriptor; anything else is only treated as a pointer when
 * its HID report descriptor is exactly the 74 bytes QEMU's usb-tablet reports,
 * because there is no HID report parser yet and guessing turns every vendor
 * HID device into a mouse. */
int usb_find_hid_pointer(const u8 *config, int length, u16 max_packet_limit,
                         const char *tag, struct usb_hid_pointer *out);

#endif /* USB_DEVICE_H */
