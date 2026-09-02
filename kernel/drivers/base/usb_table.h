#ifndef USB_TABLE_H
#define USB_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "vendors/usb_ids.h"
#include <utilities/types.h>

struct usb_device_desc {
  u16 vendor;
  u16 product;
  const char *name;
};

static const struct usb_device_desc usb_known_devices[] = {
    /* Intel */
    {USB_VENDOR_INTEL, USB_PRODUCT_INTEL_USB3_HUB, "Intel USB 3.x Hub"},

    /* Logitech */
    {USB_VENDOR_LOGITECH, USB_PRODUCT_LOGITECH_UNIFYING,
     "Logitech Unifying Receiver"},
    {USB_VENDOR_LOGITECH, USB_PRODUCT_LOGITECH_K120, "Logitech K120 Keyboard"},

    /* Microsoft */
    {USB_VENDOR_MICROSOFT, USB_PRODUCT_MICROSOFT_XBOX_360,
     "Microsoft Xbox 360 Controller"},

    /* FTDI */
    {USB_VENDOR_FTDI, USB_PRODUCT_FTDI_SERIAL, "FTDI USB Serial Converter"},

    /* Prolific */
    {USB_VENDOR_PROLIFIC, USB_PRODUCT_PROLIFIC_PL2303,
     "Prolific PL2303 USB Serial Converter"},

    /* SanDisk */
    {USB_VENDOR_SANDISK, USB_PRODUCT_SANDISK_CRUZER,
     "SanDisk Cruzer USB Flash Drive"},

    /* Kingston */
    {USB_VENDOR_KINGSTON, USB_PRODUCT_KINGSTON_DATATRAVELER,
     "Kingston DataTraveler USB Flash Drive"},

    /* QEMU */
    {USB_VENDOR_QEMU, USB_PRODUCT_QEMU_TABLET, "QEMU USB Tablet"},
};

#define USB_KNOWN_DEVICE_COUNT                                                 \
  (sizeof(usb_known_devices) / sizeof(usb_known_devices[0]))

SINLINE const struct usb_device_desc *usb_device_lookup(u16 vendor,
                                                        u16 product) {
  for (unsigned i = 0; i < USB_KNOWN_DEVICE_COUNT; ++i) {
    const struct usb_device_desc *device = &usb_known_devices[i];

    if (device->vendor == vendor && device->product == product) {
      return device;
    }
  }

  return NULL;
}

SINLINE const char *usb_device_name(u16 vendor, u16 product) {
  const struct usb_device_desc *device = usb_device_lookup(vendor, product);

  return device != NULL ? device->name : "Unknown USB device";
}

#endif
