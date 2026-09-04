#include <devices/io.h>
#include <pci/pci.h>
#include <stdint.h>
#include <utilities/log.h>

int uhci_init(struct pci_device *dev);
int ehci_init(struct pci_device *dev);
int xhci_init(struct pci_device *dev);

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB 0x03

/* PCI programming-interface values for USB controllers. */
#define USB_PROGIF_UHCI 0x00
#define USB_PROGIF_OHCI 0x10
#define USB_PROGIF_EHCI 0x20
#define USB_PROGIF_XHCI 0x30

static void usb_init_controller(struct pci_device *dev) {
  /* Enable BAR access and bus mastering before controller setup. */
  pci_enable(dev);

  switch (dev->prog_if) {
  case USB_PROGIF_UHCI:
    log_write("USB: Found UHCI controller.", KERNEL, LOG_INFO);
    uhci_init(dev);
    break;
  case USB_PROGIF_OHCI:
    log_write("USB: Found OHCI controller.", KERNEL, LOG_INFO);
    // TODO: Implement OHCI.
    break;
  case USB_PROGIF_EHCI:
    log_write("USB: Found EHCI controller.", KERNEL, LOG_INFO);
    ehci_init(dev);
    break;
  case USB_PROGIF_XHCI:
    log_write("USB: Found xHCI controller.", KERNEL, LOG_INFO);
    xhci_init(dev);
    break;
  default:
    log_write("USB: Unknown USB controller type.", KERNEL, LOG_ERROR);
    break;
  }
}

void usb_init(void) {
  log_write("USB: Initializing.", KERNEL, LOG_INFO);

  /* USB ports may be split across several controllers. */
  u32 found = 0;
  u32 count = pci_device_count();

  for (u32 i = 0; i < count; i++) {
    struct pci_device dev;
    if (!pci_device_at(i, &dev))
      continue;
    if (dev.class_code != PCI_CLASS_SERIAL_BUS ||
        dev.subclass != PCI_SUBCLASS_USB)
      continue;

    found++;

    /* Log the PCI address to diagnose controller routing. */
    log_write("USB: Found USB controller.", KERNEL, LOG_INFO);
    log_write_hex("USB:   bus", dev.addr.bus, KERNEL, LOG_INFO);
    log_write_hex("USB:   device", dev.addr.dev, KERNEL, LOG_INFO);
    log_write_hex("USB:   function", dev.addr.fn, KERNEL, LOG_INFO);
    log_write_hex("USB:   vendor", dev.vendor, KERNEL, LOG_INFO);
    log_write_hex("USB:   device id", dev.device, KERNEL, LOG_INFO);
    log_write_hex("USB:   prog_if", dev.prog_if, KERNEL, LOG_INFO);

    usb_init_controller(&dev);
  }

  if (found == 0) {
    log_write("USB: No USB controller found.", KERNEL, LOG_INFO);
    return;
  }

  log_write_int("USB: controllers initialised", (int64_t)found, KERNEL,
                LOG_INFO);
}
