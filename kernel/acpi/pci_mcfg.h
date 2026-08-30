#ifndef PCI_MCFG_H
#define PCI_MCFG_H


/* PACKED and friends. */
#include <utilities/types.h>
#include <acpi/acpi.h>
#include <stdint.h>

#define ACPI_SIG_MCFG "MCFG"
#define PCI_MCFG_MAX_RANGES 16

/* The MCFG fixed body is followed by one or more allocation entries. */
struct PACKED acpi_mcfg {
    struct acpi_sdt_header h;
    u64 reserved;
};

struct PACKED acpi_mcfg_allocation {
    u64 base_phys;
    u16 segment;
    u8  start_bus;
    u8  end_bus;
    u32 reserved;
};

/* Validated form retained by the PCI layer. One range describes a contiguous
 * ECAM aperture for a PCI segment and inclusive bus-number interval. */
struct pci_mcfg_range {
    u64 base_phys;
    u16 segment;
    u8  start_bus;
    u8  end_bus;
};

/* Parse a checksum-valid MCFG table. Passing NULL resets the range table. */
int pci_mcfg_init(const struct acpi_sdt_header *table);

u32 pci_mcfg_range_count(void);
int pci_mcfg_range_at(u32 index, struct pci_mcfg_range *out);
int pci_mcfg_find(u16 segment, u8 bus,
                  struct pci_mcfg_range *out);

#endif
