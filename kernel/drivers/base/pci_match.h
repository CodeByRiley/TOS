#ifndef PCI_MATCH_H
#define PCI_MATCH_H

#include <utilities/types.h>
#include <stdint.h>

struct pci_id {
    u16 vendor;
    u16 device;
};

SINLINE int pci_id_match(struct pci_id id,
                               u16 vendor,
                               u16 device)
{
    return id.vendor == vendor && id.device == device;
}

SINLINE int pci_id_match_exact(u16 vendor, u16 device,
                                     struct pci_id want)
{
    return vendor == want.vendor && device == want.device;
}

SINLINE int pci_id_match_vendor(u16 vendor, u16 want_vendor)
{
    return vendor == want_vendor;
}

SINLINE int pci_match_class(u8 base_class,
                                  u8 subclass,
                                  u8 prog_if,
                                  u8 dev_base_class,
                                  u8 dev_subclass,
                                  u8 dev_prog_if)
{
    return (base_class == dev_base_class &&
            subclass == dev_subclass &&
            prog_if == dev_prog_if);
}

// Convenience macro specifically for AHCI
#define PCI_IS_AHCI(base, sub, prog) \
    pci_match_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROGIF_SATA_AHCI, base, sub, prog)


#endif
