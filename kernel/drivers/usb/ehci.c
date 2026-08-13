#include <pci/pci.h>
#include <utilities/log.h>

int ehci_init(struct pci_device *dev) {
    log_write("EHCI: Driver not implemented yet.", KERNEL, LOG_WARN);
    return -1;
}
