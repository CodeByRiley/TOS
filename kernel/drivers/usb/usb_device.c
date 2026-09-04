/* kernel/drivers/usb/usb_device.c , the USB device model. See usb_device.h. */
#include <drivers/usb/usb_device.h>

#include <utilities/log.h>
#include <utilities/string.h>

static int control(const struct usb_pipe *pipe,
                   const struct usb_setup_packet *setup, void *data, u16 length,
                   int in) {
    if (!pipe || !pipe->control)
        return -1;
    return pipe->control(pipe, setup, data, length, in);
}

int usb_get_descriptor(const struct usb_pipe *pipe, u8 type, u8 index,
                       void *out, u16 length) {
    struct usb_setup_packet setup = {
        .bmRequestType = USB_REQTYPE_IN_STD_DEVICE,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (u16)((type << 8) | index),
        .wIndex = 0,
        .wLength = length,
    };
    return control(pipe, &setup, out, length, 1);
}

int usb_set_address(const struct usb_pipe *pipe, u8 address) {
    struct usb_setup_packet setup = {
        .bmRequestType = USB_REQTYPE_OUT_STD_DEVICE,
        .bRequest = USB_REQ_SET_ADDRESS,
        .wValue = address,
        .wIndex = 0,
        .wLength = 0,
    };
    /* Addressed to 0: the device adopts `address` only once status completes,
     * so the pipe must still be the default one when this is called. */
    struct usb_pipe zero = *pipe;
    zero.address = 0;
    return control(&zero, &setup, 0, 0, 0);
}

int usb_set_configuration(const struct usb_pipe *pipe, u8 configuration) {
    struct usb_setup_packet setup = {
        .bmRequestType = USB_REQTYPE_OUT_STD_DEVICE,
        .bRequest = USB_REQ_SET_CONFIGURATION,
        .wValue = configuration,
        .wIndex = 0,
        .wLength = 0,
    };
    return control(pipe, &setup, 0, 0, 0);
}

static int hid_request(const struct usb_pipe *pipe, u8 request, u16 value,
                       u8 interface) {
    struct usb_setup_packet setup = {
        .bmRequestType = USB_REQTYPE_OUT_CLASS_INTERFACE,
        .bRequest = request,
        .wValue = value,
        .wIndex = interface,
        .wLength = 0,
    };
    return control(pipe, &setup, 0, 0, 0);
}

int usb_hid_set_idle(const struct usb_pipe *pipe, u8 interface) {
    return hid_request(pipe, USB_REQ_SET_IDLE, 0, interface);
}

int usb_hid_set_boot_protocol(const struct usb_pipe *pipe, u8 interface) {
    return hid_request(pipe, USB_REQ_SET_PROTOCOL, 0 /* boot */, interface);
}

int usb_read_config(const struct usb_pipe *pipe, u8 index, u8 *buffer,
                    u16 capacity) {
    if (!buffer || capacity < sizeof(struct usb_config_descriptor))
        return -1;
    const char *tag = pipe && pipe->tag ? pipe->tag : "USB";

    struct usb_config_descriptor head;
    memset(&head, 0, sizeof(head));
    if (usb_get_descriptor(pipe, USB_DESC_CONFIGURATION, index, &head,
                           sizeof(head)) < (int)sizeof(head)) {
        log_write_string("config header read failed on", tag, KERNEL,
                         LOG_ERROR);
        return -1;
    }

    u16 total = head.wTotalLength;
    if (total < sizeof(head)) {
        log_write_int("USB: config wTotalLength nonsensical", total, KERNEL,
                      LOG_ERROR);
        return -1;
    }
    if (total > capacity) {
        log_write_int("USB: config block too large, truncating", total, KERNEL,
                      LOG_WARN);
        total = capacity;
    }

    memset(buffer, 0, capacity);
    int got = usb_get_descriptor(pipe, USB_DESC_CONFIGURATION, index, buffer,
                                 total);
    if (got < (int)sizeof(head)) {
        log_write_int("USB: config block read failed, got", got, KERNEL,
                      LOG_ERROR);
        return -1;
    }
    return got;
}

int usb_find_hid_pointer(const u8 *config, int length, u16 max_packet_limit,
                         const char *tag, struct usb_hid_pointer *out) {
    if (!config || !out)
        return 0;
    (void)tag;

    int found = 0;
    int hid_interface = 0;
    u8 current_interface = 0;
    u8 current_kind = USB_INT_KIND_NONE;

    for (int offset = 0; offset + 2 <= length;) {
        u8 descriptor_length = config[offset];
        u8 descriptor_type = config[offset + 1];

        /* A zero length would spin forever, and running past the end means the
         * device lied about wTotalLength. Stop either way. */
        if (descriptor_length < 2 || offset + descriptor_length > length)
            break;

        if (descriptor_type == USB_DESC_INTERFACE &&
            descriptor_length >= sizeof(struct usb_interface_descriptor)) {
            const struct usb_interface_descriptor *interface =
                (const struct usb_interface_descriptor *)(const void *)(config +
                                                                       offset);
            current_interface = interface->bInterfaceNumber;
            hid_interface = interface->bInterfaceClass == USB_CLASS_HID;
            current_kind = USB_INT_KIND_NONE;
            if (hid_interface &&
                interface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                interface->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE)
                current_kind = USB_INT_KIND_HID_BOOT_MOUSE;
            log_write_int("USB:   interface", current_interface, KERNEL,
                          LOG_INFO);
            log_write_hex("USB:     class", interface->bInterfaceClass, KERNEL,
                          LOG_INFO);
            log_write_hex("USB:     subclass", interface->bInterfaceSubClass,
                          KERNEL, LOG_INFO);
            log_write_hex("USB:     protocol", interface->bInterfaceProtocol,
                          KERNEL, LOG_INFO);
            log_write_int("USB:     endpoints", interface->bNumEndpoints,
                          KERNEL, LOG_INFO);
        } else if (descriptor_type == USB_DESC_HID && descriptor_length >= 9) {
            u16 report_length = (u16)config[offset + 7] |
                                ((u16)config[offset + 8] << 8);
            log_write_int("USB:   HID report length", report_length, KERNEL,
                          LOG_INFO);
            if (hid_interface && current_kind == USB_INT_KIND_NONE &&
                report_length == 74)
                current_kind = USB_INT_KIND_HID_TABLET;
        } else if (descriptor_type == USB_DESC_ENDPOINT &&
                   descriptor_length >= sizeof(struct usb_endpoint_descriptor)) {
            const struct usb_endpoint_descriptor *endpoint =
                (const struct usb_endpoint_descriptor *)(const void *)(config +
                                                                      offset);
            u16 maxpacket = endpoint->wMaxPacketSize & 0x07FF;
            log_write_hex("USB:   endpoint addr", endpoint->bEndpointAddress,
                          KERNEL, LOG_INFO);
            log_write_hex("USB:     attributes", endpoint->bmAttributes, KERNEL,
                          LOG_INFO);
            log_write_int("USB:     max packet", maxpacket, KERNEL, LOG_INFO);
            log_write_int("USB:     interval", endpoint->bInterval, KERNEL,
                          LOG_INFO);

            /* A tablet reports absolute coordinates, so its packets have to be
             * big enough to carry them. */
            int size_ok = current_kind != USB_INT_KIND_HID_TABLET ||
                          maxpacket >= 5;
            int fits_buffer = !max_packet_limit || maxpacket <= max_packet_limit;
            if (!found && current_kind != USB_INT_KIND_NONE && size_ok &&
                fits_buffer &&
                (endpoint->bmAttributes & USB_EP_XFER_MASK) ==
                    USB_EP_XFER_INTERRUPT &&
                (endpoint->bEndpointAddress & USB_EP_ADDR_DIR_IN)) {
                out->interface_number = current_interface;
                out->kind = current_kind;
                out->endpoint = *endpoint;
                out->endpoint.wMaxPacketSize = maxpacket;
                found = 1;
            }
        }

        offset += descriptor_length;
    }

    return found;
}
