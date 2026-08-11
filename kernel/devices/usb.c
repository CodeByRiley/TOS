#include "pci/pci.h"
#include "devices/io.h"
#include "utilities/log.h"
#include <stdint.h>

// Forward declarations of your future HCD init functions
int uhci_init(struct pci_device *dev);
int ehci_init(struct pci_device *dev);
int xhci_init(struct pci_device *dev);

void usb_init(void) {
    struct pci_device dev;
    log_write("USB: Initializing.", KERNEL, LOG_INFO);

    // Class 0x0C (Serial Bus), Subclass 0x03 (USB)
    if (!pci_find_by_class(0x0C, 0x03, &dev)) {
        log_write("USB: No USB controller found.", KERNEL, LOG_INFO);
        return;
    }

    log_write("USB: Found USB controller.", KERNEL, LOG_INFO);
    log_write("USB: Enabling MMIO/IO and Bus Mastering.", KERNEL, LOG_INFO);
    // Tell the PCI bus to enable MMIO/IO and Bus Mastering (DMA)
    pci_enable(&dev);

    // The Prog IF tells us exactly which hardware standard to use
    switch (dev.prog_if) {
        case 0x00:
            log_write("USB: Found UHCI controller.", KERNEL, LOG_INFO);
            uhci_init(&dev);
            break;
        case 0x10:
            log_write("USB: Found OHCI controller.", KERNEL, LOG_INFO);
            // OHCI not implemented yet
            break;
        case 0x20:
            log_write("USB: Found EHCI controller.", KERNEL, LOG_INFO);
            ehci_init(&dev);
            break;
        case 0x30:
            log_write("USB: Found xHCI controller.", KERNEL, LOG_INFO);
            xhci_init(&dev);
            break;
        default:
            log_write("USB: Unknown USB controller type.", KERNEL, LOG_ERROR);
            break;
    }
}
