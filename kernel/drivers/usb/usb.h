// kernel/drivers/usb/usb.h
//
// Shared USB definitions: wire descriptors, device/URB model, HCD interface,
// class driver registration. Implemented by usb/core.c.
//
// Wire-format structs are packed and little-endian; fields read directly
// on x86/x86_64.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- speed ------------------------------------------------------------ */

typedef enum {
  USB_SPEED_LOW = 0,   /* 1.5 Mbit/s */
  USB_SPEED_FULL = 1,  /* 12  Mbit/s */
  USB_SPEED_HIGH = 2,  /* 480 Mbit/s */
  USB_SPEED_SUPER = 3, /* 5   Gbit/s */
} usb_speed_t;

/* ep0 max packet size mandated per speed (refine from bMaxPacketSize0 later) */
static inline uint16_t usb_ep0_mps(usb_speed_t speed) {
  switch (speed) {
  case USB_SPEED_LOW:
    return 8;
  case USB_SPEED_HIGH:
    return 64;
  case USB_SPEED_SUPER:
    return 512;
  default:
    return 64; /* full */
  }
}

/* ---- transfers --------------------------------------------------------- */

typedef enum {
  USB_XFER_CONTROL = 0,
  USB_XFER_ISO = 1,
  USB_XFER_BULK = 2,
  USB_XFER_IRQ = 3,
} usb_xfer_type_t;

#define USB_DIR_OUT 0x00u
#define USB_DIR_IN 0x80u

#define USB_EP_ADDR(num, dir) ((uint8_t)((num) | (dir)))
#define USB_EP_NUM(a) ((a) & 0x0Fu)
#define USB_EP_DIR(a) ((a) & 0x80u)

/* ---- wire descriptors --------------------------------------------------- */

enum {
  USB_DESC_DEVICE = 0x01,
  USB_DESC_CONFIG = 0x02,
  USB_DESC_STRING = 0x03,
  USB_DESC_INTERFACE = 0x04,
  USB_DESC_ENDPOINT = 0x05,
  USB_DESC_BOS = 0x0F,
  USB_DESC_DEV_CAP = 0x10,
  USB_DESC_EP_COMPANION = 0x30, /* SuperSpeed */
};

typedef struct __attribute__((packed)) {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t bcdUSB;
  uint8_t bDeviceClass;
  uint8_t bDeviceSubClass;
  uint8_t bDeviceProtocol;
  uint8_t bMaxPacketSize0;
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t iManufacturer;
  uint8_t iProduct;
  uint8_t iSerialNumber;
  uint8_t bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t wTotalLength;
  uint8_t bNumInterfaces;
  uint8_t bConfigurationValue;
  uint8_t iConfiguration;
  uint8_t bmAttributes;
  uint8_t bMaxPower; /* units of 2 mA */
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bNumEndpoints;
  uint8_t bInterfaceClass;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;
  uint8_t iInterface;
} usb_iface_desc_t;

typedef struct __attribute__((packed)) {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bEndpointAddress;
  uint8_t bmAttributes; /* bits 0..1 = transfer type */
  uint16_t wMaxPacketSize;
  uint8_t bInterval; /* raw; semantics differ per speed */
} usb_ep_desc_t;

typedef struct __attribute__((packed)) {
  uint8_t bmRequestType; /* D7 dir, D6..5 type, D4..0 recipient */
  uint8_t bRequest;
  uint16_t wValue;
  uint16_t wIndex;
  uint16_t wLength;
} usb_setup_t;

enum {
  USB_REQ_GET_STATUS = 0x00,
  USB_REQ_CLEAR_FEATURE = 0x01,
  USB_REQ_SET_FEATURE = 0x03,
  USB_REQ_SET_ADDRESS = 0x05,
  USB_REQ_GET_DESCRIPTOR = 0x06,
  USB_REQ_GET_CONFIG = 0x08,
  USB_REQ_SET_CONFIG = 0x09,
  USB_REQ_GET_INTERFACE = 0x0A,
  USB_REQ_SET_INTERFACE = 0x0B,
};

enum { USB_FEATURE_ENDPOINT_HALT = 0x00 };

#define USB_REQ_TYPE_STD 0x00u
#define USB_REQ_TYPE_CLASS 0x20u
#define USB_REQ_TYPE_VENDOR 0x40u

/* ---- status codes ------------------------------------------------------- */

typedef enum {
  USB_OK = 0,
  USB_STALL, /* endpoint halted; recover with usb_clear_halt() */
  USB_NAK,
  USB_BABBLE, /* device sent more than the buffer */
  USB_TIMEOUT,
  USB_NO_DEVICE,
  USB_IOERR, /* CRC / bit-stuff / bus timeout */
  USB_CANCELED,
  USB_INVAL,
} usb_status_t;

/* ---- parsed runtime structures ------------------------------------------ */

struct usb_hcd;
struct usb_class_driver;

typedef struct {
  uint8_t address; /* full bEndpointAddress: num | USB_DIR_* */
  usb_xfer_type_t type;
  uint16_t max_packet;
  uint8_t interval; /* raw bInterval */
} usb_ep_t;

typedef struct {
  uint8_t number;
  uint8_t alt;
  uint8_t cls;
  uint8_t subclass;
  uint8_t protocol;
  uint8_t num_eps;
  usb_ep_t *eps; /* endpoints of the active alt setting */
} usb_iface_t;

typedef struct {
  usb_config_desc_t *desc; /* points into raw */
  void *raw;               /* whole config blob (ifaces + eps inside) */
  size_t raw_len;
  usb_iface_t *ifaces;
  uint8_t num_ifaces;
} usb_config_t;

typedef struct usb_device {
  struct usb_hcd *hcd;
  uint8_t port;    /* root hub port on hcd */
  uint8_t address; /* 0 until assigned */
  usb_speed_t speed;

  usb_device_desc_t desc; /* cached */
  usb_ep_t ep0;           /* filled in by core */
  usb_config_t config;    /* active configuration */

  const struct usb_class_driver *driver;
  void *priv; /* bound class driver's state */
} usb_device_t;

/* ---- URB ---------------------------------------------------------------- */

enum {
  USB_URB_ZLP = 1u << 0
}; /* OUT: append ZLP when length % max_packet == 0 */

typedef struct usb_urb {
  usb_device_t *dev;
  usb_ep_t *ep;       /* NULL = ep0 (control) */
  usb_setup_t *setup; /* control transfers only; DMA-safe */
  void *buffer;
  size_t length; /* buffer size: IN fills it, OUT sends it */
  size_t actual; /* valid after completion */
  uint32_t flags;
  usb_status_t status;

  void (*complete)(struct usb_urb *urb); /* runs in HCD IRQ context */
  void *ctx; /* caller's cookie, handed back untouched */

  void *hc_priv; /* HCD-private (ring slots etc.); core never touches */
} usb_urb_t;

/* ---- HCD (host controller driver) interface ------------------------------ */

typedef struct {
  bool connected;
  bool enabled; /* reset complete, transfers allowed */
  bool powered;
  usb_speed_t speed; /* valid once connected */
} usb_port_status_t;

typedef struct usb_hcd {
  const struct usb_hcd_ops *ops;
  void *priv; /* register bases, rings, ... */
  int index;  /* assigned by core on registration */
} usb_hcd_t;

typedef struct usb_hcd_ops {
  const char *name; /* "xhci", "ehci", "uhci" */

  int (*start)(usb_hcd_t *hcd);

  /* Async submit. 0 = accepted; on failure return nonzero and do NOT
   * complete. Must call usb_urb_complete() exactly once per accepted URB. */
  int (*submit)(usb_hcd_t *hcd, usb_urb_t *urb);

  /* Optional: best-effort cancel; complete with USB_CANCELED. */
  int (*cancel)(usb_hcd_t *hcd, usb_urb_t *urb);

  /* Root hub ports. No virtual hub device -- the core calls these
   * directly during enumeration. */
  int (*port_count)(usb_hcd_t *hcd);
  usb_port_status_t (*port_status)(usb_hcd_t *hcd, uint8_t port);
  int (*port_reset)(usb_hcd_t *hcd, uint8_t port); /* block until enabled */

  /* Optional: xHCI assigns addresses via the Address Device command, not
   * a SET_ADDRESS transfer. If provided, set dev->address and return 0;
   * the core skips the control transfer. */
  int (*assign_address)(usb_hcd_t *hcd, usb_device_t *dev);
} usb_hcd_ops_t;

/* ---- class drivers ------------------------------------------------------- */

#define USB_IFACE_MATCHES(ifc, c, sc, p)                                       \
  ((ifc)->cls == (c) && (ifc)->subclass == (sc) && (ifc)->protocol == (p))

typedef struct usb_class_driver {
  const char *name;
  bool (*match)(const usb_device_t *dev, const usb_iface_t *ifc);
  int (*probe)(usb_device_t *dev, usb_iface_t *ifc); /* set dev->priv */
  void (*remove)(usb_device_t *dev);
  struct usb_class_driver *next; /* core-managed list link */
} usb_class_driver_t;

/* ---- core API ----------------------------------------------- */

void usb_init(void);
int usb_hcd_register(usb_hcd_t *hcd); /* probes all ports after this */
int usb_class_register(usb_class_driver_t *drv);

/* HCDs call this from port-change IRQ/polling. Core reads port_status() and
 * enumerates or tears down. On disconnect the HCD must already have completed
 * every pending URB for that device with USB_NO_DEVICE. */
void usb_port_event(usb_hcd_t *hcd, uint8_t port);

/* HCDs call exactly once per accepted URB. Sets status/actual, then runs
 * urb->complete(). */
void usb_urb_complete(usb_urb_t *urb, usb_status_t status, size_t actual);

/* Async submit; 0 if accepted. */
int usb_submit(usb_urb_t *urb);

/* Synchronous wrappers -- block the caller, never use from IRQ context. */
usb_status_t usb_control(usb_device_t *dev, const usb_setup_t *setup, void *buf,
                         size_t len, uint32_t timeout_ms);
usb_status_t usb_bulk(usb_device_t *dev, usb_ep_t *ep, void *buf, size_t len,
                      size_t *actual, uint32_t timeout_ms);
usb_status_t usb_interrupt(usb_device_t *dev, usb_ep_t *ep, void *buf,
                           size_t len, uint32_t timeout_ms);

/* Conveniences layered on usb_control(). */
usb_status_t usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                                uint16_t lang, void *buf, size_t len);
usb_status_t usb_get_string_ascii(usb_device_t *dev, uint8_t index, char *out,
                                  size_t out_len);
usb_status_t usb_set_config(usb_device_t *dev, uint8_t config_value);
usb_status_t usb_clear_halt(usb_device_t *dev, usb_ep_t *ep);
