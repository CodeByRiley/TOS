#ifndef USB_MATCH_H
#define USB_MATCH_H

#include <stdint.h>
#include <utilities/types.h>

/*
 * USB wildcard value.
 *
 * This value means that the corresponding field is ignored while matching.
 */
#define USB_MATCH_ANY 0xFF

struct usb_id {
  u16 vendor;
  u16 product;
};

struct usb_class_id {
  u8 class;
  u8 subclass;
  u8 protocol;
};

SINLINE int usb_id_match(struct usb_id id, u16 vendor, u16 product) {
  return id.vendor == vendor && id.product == product;
}

SINLINE int usb_id_match_exact(u16 vendor, u16 product, struct usb_id want) {
  return vendor == want.vendor && product == want.product;
}

SINLINE int usb_id_match_vendor(u16 vendor, u16 want_vendor) {
  return vendor == want_vendor;
}

SINLINE int usb_match_class(u8 dev_class, u8 dev_subclass, u8 dev_protocol,
                            u8 want_class, u8 want_subclass, u8 want_protocol) {
  return dev_class == want_class && dev_subclass == want_subclass &&
         dev_protocol == want_protocol;
}

SINLINE int usb_match_class_wildcard(u8 dev_class, u8 dev_subclass,
                                     u8 dev_protocol, u8 want_class,
                                     u8 want_subclass, u8 want_protocol) {
  return (want_class == USB_MATCH_ANY || dev_class == want_class) &&
         (want_subclass == USB_MATCH_ANY || dev_subclass == want_subclass) &&
         (want_protocol == USB_MATCH_ANY || dev_protocol == want_protocol);
}

/*
 * USB device/interface classes.
 */
#define USB_CLASS_PER_INTERFACE 0x00
#define USB_CLASS_AUDIO 0x01
#define USB_CLASS_COMM 0x02
#define USB_CLASS_HID 0x03
#define USB_CLASS_PHYSICAL 0x05
#define USB_CLASS_IMAGE 0x06
#define USB_CLASS_PRINTER 0x07
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB 0x09
#define USB_CLASS_CDC_DATA 0x0A
#define USB_CLASS_SMART_CARD 0x0B
#define USB_CLASS_VIDEO 0x0E
#define USB_CLASS_WIRELESS 0xE0
#define USB_CLASS_MISCELLANEOUS 0xEF
#define USB_CLASS_VENDOR_SPEC 0xFF

/*
 * HID subclasses and protocols.
 */
#define USB_HID_SUBCLASS_NONE 0x00
#define USB_HID_SUBCLASS_BOOT 0x01

#define USB_HID_PROTOCOL_NONE 0x00
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE 0x02

/*
 * Mass-storage subclasses and protocols.
 */
#define USB_MASS_SUBCLASS_RBC 0x01
#define USB_MASS_SUBCLASS_ATAPI 0x02
#define USB_MASS_SUBCLASS_UFI 0x04
#define USB_MASS_SUBCLASS_SCSI 0x06
#define USB_MASS_SUBCLASS_VENDOR_SPEC 0xFF

#define USB_MASS_PROTOCOL_CBI 0x00
#define USB_MASS_PROTOCOL_CBI_NO_INTERRUPT 0x01
#define USB_MASS_PROTOCOL_BULK_ONLY 0x50

/*
 * HID matching.
 */
#define USB_IS_HID(class, sub, proto)                                          \
  usb_match_class_wildcard((class), (sub), (proto), USB_CLASS_HID,             \
                           USB_MATCH_ANY, USB_MATCH_ANY)

#define USB_IS_KEYBOARD(class, sub, proto)                                     \
  usb_match_class((class), (sub), (proto), USB_CLASS_HID,                      \
                  USB_HID_SUBCLASS_BOOT, USB_HID_PROTOCOL_KEYBOARD)

#define USB_IS_MOUSE(class, sub, proto)                                        \
  usb_match_class((class), (sub), (proto), USB_CLASS_HID,                      \
                  USB_HID_SUBCLASS_BOOT, USB_HID_PROTOCOL_MOUSE)

/*
 * Hub matching.
 */
#define USB_IS_HUB(class, sub, proto)                                          \
  usb_match_class_wildcard((class), (sub), (proto), USB_CLASS_HUB,             \
                           USB_MATCH_ANY, USB_MATCH_ANY)

/*
 * Mass-storage matching.
 *
 * USB_IS_MASS_STORAGE() accepts any mass-storage subclass and protocol.
 *
 * USB_IS_SCSI_MASS_STORAGE() accepts the SCSI transparent command set
 * with any transport protocol.
 *
 * USB_IS_BULK_ONLY_STORAGE() matches the most common USB storage type:
 * SCSI transparent subclass with Bulk-Only Transport.
 */
#define USB_IS_MASS_STORAGE(class, sub, proto)                                 \
  usb_match_class_wildcard((class), (sub), (proto), USB_CLASS_MASS_STORAGE,    \
                           USB_MATCH_ANY, USB_MATCH_ANY)

#define USB_IS_SCSI_MASS_STORAGE(class, sub, proto)                            \
  usb_match_class_wildcard((class), (sub), (proto), USB_CLASS_MASS_STORAGE,    \
                           USB_MASS_SUBCLASS_SCSI, USB_MATCH_ANY)

#define USB_IS_BULK_ONLY_STORAGE(class, sub, proto)                            \
  usb_match_class((class), (sub), (proto), USB_CLASS_MASS_STORAGE,             \
                  USB_MASS_SUBCLASS_SCSI, USB_MASS_PROTOCOL_BULK_ONLY)

#endif
