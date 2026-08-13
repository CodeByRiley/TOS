#ifndef PCI_MCFG_H
#define PCI_MCFG_H

#include <acpi/acpi.h>
#include <stdint.h>

#define ACPI_SIG_MCFG "MCFG"
#define PCI_MCFG_MAX_RANGES 16

/* The MCFG fixed body is followed by one or more allocation entries. */
struct __attribute__((packed)) acpi_mcfg {
    struct acpi_sdt_header h;
    uint64_t reserved;
};

struct __attribute__((packed)) acpi_mcfg_allocation {
    uint64_t base_phys;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
};

/* Validated form retained by the PCI layer. One range describes a contiguous
 * ECAM aperture for a PCI segment and inclusive bus-number interval. */
struct pci_mcfg_range {
    uint64_t base_phys;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
};

/* Parse a checksum-valid MCFG table. Passing NULL resets the range table. */
int pci_mcfg_init(const struct acpi_sdt_header *table);

uint32_t pci_mcfg_range_count(void);
int pci_mcfg_range_at(uint32_t index, struct pci_mcfg_range *out);
int pci_mcfg_find(uint16_t segment, uint8_t bus,
                  struct pci_mcfg_range *out);

#endif
