/* kernel/devices/usb.h -
 * USB Core stack definitions.
 */

#ifndef USB_H
#define USB_H

#include "devices/io.h"
#include "utilities/stdlib.h"
#include "utilities/types.h"

enum usb_speed {
    USB_LOW_SPEED,   // 1.5 Mbps
    USB_FULL_SPEED,  // 12 Mbps (USB 1.1)
    USB_HIGH_SPEED,  // 480 Mbps (USB 2.0)
    USB_SUPER_SPEED  // 5 Gbps+ (USB 3.0+)
};

/* The Host Controller Driver (HCD) abstraction layer */
struct hcd {
    // The name of the controller (e.g., "UHCI", "xHCI")
    char *name;

    // The I/O base or MMIO base address for this controller
    uint32_t base_address;

    // --- Function Pointers (The HCD API) ---

    // Initialize the hardware controller
    int  (*init)(struct hcd *hcd);

    // Check if a device is physically plugged into a specific port
    int  (*detect_port)(struct hcd *hcd, uint8_t port);

    // Reset a port so a device is ready to receive commands
    void (*reset_port)(struct hcd *hcd, uint8_t port);

    // Send a control transfer (Setup Packet + Data)
    int  (*control_transfer)(struct hcd *hcd, uint8_t device_addr,
                             uint8_t endpoint, void *setup,
                             void *buffer, uint16_t length);
};

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
} __attribute__((packed));

struct usb_device {
	struct usb_descriptor desc;
	enum usb_speed speed;
	u8 addr;
	u8 port;
	struct hcd *hcd;
};

void usb_init(void);

/* Function prototypes to be implemented in usb.c */

/**
 * @brief Called by the HCD when it detects a device has been plugged in.
 * @param hcd The host controller driver that detected the connection.
 * @param port The port number the device was plugged into.
 */
void usb_register_device(struct hcd *hcd, uint8_t port);

/**
 * @brief Enumerates a newly connected device.
 * Assigns an address, retrieves the device descriptor, and sets configuration.
 * @param dev Pointer to the usb_device struct.
 * @return 0 on success, negative error code on failure.
 */
int usb_enumerate_device(struct usb_device *dev);

/**
 * @brief Sends a control transfer to a device.
 * This is the fundamental way you talk to a USB device to get descriptors,
 * set addresses, and set configurations.
 */
int usb_control_transfer(struct usb_device *dev, void *setup_packet, void *buffer, uint16_t length);

#endif /* USB_H */
