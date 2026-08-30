/* USB core types. */

#ifndef USB_H
#define USB_H

#include <devices/io.h>
#include <utilities/stdlib.h>
#include <utilities/types.h>

enum usb_speed {
    USB_LOW_SPEED,   // 1.5 Mbps
    USB_FULL_SPEED,  // 12 Mbps (USB 1.1)
    USB_HIGH_SPEED,  // 480 Mbps (USB 2.0)
    USB_SUPER_SPEED  // 5 Gbps+ (USB 3.0+)
};

/* Host-controller interface. */
struct hcd {
    char *name;
    u32 base_address;

    int  (*init)(struct hcd *hcd);
    int  (*detect_port)(struct hcd *hcd, u8 port);
    void (*reset_port)(struct hcd *hcd, u8 port);
    int  (*control_transfer)(struct hcd *hcd, u8 device_addr,
                             u8 endpoint, void *setup,
                             void *buffer, u16 length);
};

/* Setup packet , the 8 bytes opening every control transfer. USB is
 * little-endian like x86, so this maps straight onto the wire. */
struct usb_setup_packet {
	u8  bmRequestType;
	u8  bRequest;
	u16 wValue;
	u16 wIndex;
	u16 wLength;
} PACKED;

/* bmRequestType */
//
// Bits | Name      | Description
// 7    | Direction | 0 = host to device, 1 = device to host
// 6-5  | Type      | 0 = standard, 1 = class, 2 = vendor
// 4-0  | Recipient | 0 = device, 1 = interface, 2 = endpoint
//
// Enumeration uses standard device requests; HID setup adds class/interface.
#define USB_REQTYPE_OUT_STD_DEVICE       0x00
#define USB_REQTYPE_IN_STD_DEVICE        0x80
#define USB_REQTYPE_OUT_CLASS_INTERFACE  0x21

/* Standard bRequest codes (subset: what enumeration issues). */
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_SET_CONFIGURATION   0x09

/* HID class bRequest codes. */
#define USB_REQ_SET_IDLE            0x0A
#define USB_REQ_SET_PROTOCOL        0x0B

/* Descriptor types , high byte of wValue in GET_DESCRIPTOR. INTERFACE and
 * ENDPOINT are never requested directly; they arrive inline in the
 * configuration block. */
#define USB_DESC_DEVICE             0x01
#define USB_DESC_CONFIGURATION      0x02
#define USB_DESC_STRING             0x03
#define USB_DESC_INTERFACE          0x04
#define USB_DESC_ENDPOINT           0x05
#define USB_DESC_HID                0x21
#define USB_DESC_REPORT             0x22

/* Class codes. Device-level 0 means "the interfaces declare it", which is
 * what HID and composite devices do. */
#define USB_CLASS_PER_INTERFACE     0x00
#define USB_CLASS_HID               0x03

/* HID boot-interface subclass/protocol values. */
#define USB_HID_SUBCLASS_BOOT       0x01
#define USB_HID_PROTOCOL_KEYBOARD   0x01
#define USB_HID_PROTOCOL_MOUSE      0x02

/* bEndpointAddress */
//
// Bits | Name      | Description
// 7    | Direction | 0 = OUT, 1 = IN (ignored on control endpoints)
// 6-4  | Reserved  |
// 3-0  | Number    | Endpoint number
#define USB_EP_ADDR_NUM_MASK        0x0F
#define USB_EP_ADDR_DIR_IN          0x80

/* Endpoint bmAttributes */
//
// Bits | Name          | Description
// 7-2  | Reserved      | (isochronous endpoints use 5-2 for sync/usage type)
// 1-0  | Transfer Type | 0 = control, 1 = isochronous, 2 = bulk, 3 = interrupt
#define USB_EP_XFER_MASK            0x03
#define USB_EP_XFER_CONTROL         0x00
#define USB_EP_XFER_ISOCHRONOUS     0x01
#define USB_EP_XFER_BULK            0x02
#define USB_EP_XFER_INTERRUPT       0x03

/* Standard USB Device Descriptor (18 bytes) */
struct usb_descriptor {
	u8 bLength;
	u8 bDescriptorType;
	u16 bcdUSB;
	u8 bDeviceClass;
	u8 bDeviceSubClass;
	u8 bDeviceProtocol;
	u8 bMaxPacketSize0;
	u16 idVendor;
	u16 idProduct;
	u16 bcdDevice;
	u8 iManufacturer;
	u8 iProduct;
	u8 iSerialNumber;
	u8 bNumConfigurations;
} PACKED;

/* Configuration Descriptor (9 bytes). wTotalLength covers this descriptor plus
 * every interface and endpoint descriptor after it, returned as one contiguous
 * block , hence the two-request read: 9 bytes for the total, then the rest. */
struct usb_config_descriptor {
	u8  bLength;
	u8  bDescriptorType;
	u16 wTotalLength;
	u8  bNumInterfaces;
	u8  bConfigurationValue;
	u8  iConfiguration;
	u8  bmAttributes;
	u8  bMaxPower;
} PACKED;

/* Interface Descriptor (9 bytes). */
struct usb_interface_descriptor {
	u8 bLength;
	u8 bDescriptorType;
	u8 bInterfaceNumber;
	u8 bAlternateSetting;
	u8 bNumEndpoints;
	u8 bInterfaceClass;
	u8 bInterfaceSubClass;
	u8 bInterfaceProtocol;
	u8 iInterface;
} PACKED;

/* Endpoint Descriptor (7 bytes). */
struct usb_endpoint_descriptor {
	u8  bLength;
	u8  bDescriptorType;
	u8  bEndpointAddress;
	u8  bmAttributes;
	u16 wMaxPacketSize;
	u8  bInterval;
} PACKED;

struct usb_device {
	struct usb_descriptor desc;
	enum usb_speed speed;
	u8 addr;
	u8 port;
	struct hcd *hcd;
};

void usb_init(void);

/* Register a device found by a host controller. */
void usb_register_device(struct hcd *hcd, u8 port);

/* Assign an address and configure a device. */
int usb_enumerate_device(struct usb_device *dev);

/* Send a control transfer. */
int usb_control_transfer(struct usb_device *dev, void *setup_packet, void *buffer, u16 length);

#endif /* USB_H */
