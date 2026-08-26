#ifndef PCI_MATCH_H
#define PCI_MATCH_H

#include "utilities/types.h"
#include <stdint.h>

struct pci_id {
    uint16_t vendor;
    uint16_t device;
};

SINLINE int pci_id_match(struct pci_id id,
                               uint16_t vendor,
                               uint16_t device)
{
    return id.vendor == vendor && id.device == device;
}

SINLINE int pci_id_match_exact(uint16_t vendor, uint16_t device,
                                     struct pci_id want)
{
    return vendor == want.vendor && device == want.device;
}

SINLINE int pci_id_match_vendor(uint16_t vendor, uint16_t want_vendor)
{
    return vendor == want_vendor;
}

SINLINE int pci_match_class(uint8_t base_class,
                                  uint8_t subclass,
                                  uint8_t prog_if,
                                  uint8_t dev_base_class,
                                  uint8_t dev_subclass,
                                  uint8_t dev_prog_if)
{
    return (base_class == dev_base_class &&
            subclass == dev_subclass &&
            prog_if == dev_prog_if);
}

// Convenience macro specifically for AHCI
#define PCI_IS_AHCI(base, sub, prog) \
    pci_match_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROGIF_SATA_AHCI, base, sub, prog)


#endif
