#include <pci/pci.h>
#include <utilities/log.h>

int xhci_init(struct pci_device *dev) {
  log_write("xHCI: Driver not implemented yet.", KERNEL, LOG_WARN);
  return -1;
}
