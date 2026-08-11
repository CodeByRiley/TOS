#ifndef UHCI_H
#define UHCI_H

#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRBASEADD  0x08
#define UHCI_FRNUM      0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

int uhci_init(struct pci_device *dev);

#endif // UHCI_H
