#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI Configuration Space header offsets (header type 0). */
#define PCI_CFG_VENDOR_ID       0x00
#define PCI_CFG_DEVICE_ID       0x02
#define PCI_CFG_COMMAND         0x04
#define PCI_CFG_STATUS          0x06
#define PCI_CFG_REVISION        0x08
#define PCI_CFG_PROG_IF         0x09
#define PCI_CFG_SUBCLASS        0x0A
#define PCI_CFG_CLASS           0x0B
#define PCI_CFG_CACHE_LINE      0x0C
#define PCI_CFG_LATENCY         0x0D
#define PCI_CFG_HEADER_TYPE     0x0E
#define PCI_CFG_BIST            0x0F
#define PCI_CFG_BAR0            0x10
#define PCI_CFG_BAR1            0x14
#define PCI_CFG_BAR2            0x18
#define PCI_CFG_BAR3            0x1C
#define PCI_CFG_BAR4            0x20
#define PCI_CFG_BAR5            0x24
#define PCI_CFG_SUBSYS_VENDOR   0x2C
#define PCI_CFG_SUBSYS_ID       0x2E
#define PCI_CFG_CAP_PTR         0x34
#define PCI_CFG_INT_LINE        0x3C
#define PCI_CFG_INT_PIN         0x3D

/* PCI command register bits. */
#define PCI_CMD_IO              (1u << 0)
#define PCI_CMD_MEM             (1u << 1)
#define PCI_CMD_BUS_MASTER      (1u << 2)
#define PCI_CMD_INT_DISABLE     (1u << 10)

/* PCI status register bits. */
#define PCI_STATUS_CAP_LIST     (1u << 4)

/* BAR layout bits. */
#define PCI_BAR_IO              (1u << 0)
#define PCI_BAR_TYPE_MASK       (3u << 1)
#define PCI_BAR_TYPE_32         (0u << 1)
#define PCI_BAR_TYPE_64         (2u << 1)
#define PCI_BAR_PREFETCH        (1u << 3)

struct pci_addr {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
};

struct pci_bar {
    uint64_t base;      /* phys base, mask bits stripped */
    uint64_t size;      /* region size in bytes */
    uint8_t  is_io;     /* 1 = I/O port BAR, 0 = MMIO */
    uint8_t  is_64;     /* 1 = 64-bit MMIO BAR (consumes two slots) */
    uint8_t  prefetch;
    uint8_t  valid;     /* 1 if populated */
};

struct pci_device {
    struct pci_addr addr;
    uint16_t vendor;
    uint16_t device;
    uint16_t subsys_vendor;
    uint16_t subsys_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  int_line;
    uint8_t  int_pin;
    uint8_t  cap_ptr;       /* offset of first capability, 0 if none */
    struct pci_bar bar[6];
};

/* Raw config-space accessors. */
uint32_t pci_cfg_read32(struct pci_addr a, uint8_t off);
uint16_t pci_cfg_read16(struct pci_addr a, uint8_t off);
uint8_t  pci_cfg_read8 (struct pci_addr a, uint8_t off);
void     pci_cfg_write32(struct pci_addr a, uint8_t off, uint32_t val);
void     pci_cfg_write16(struct pci_addr a, uint8_t off, uint16_t val);
void     pci_cfg_write8 (struct pci_addr a, uint8_t off, uint8_t  val);

/* Brute-force scan all 256 busses * 32 devs * 8 fns. Populates internal table.
 * Idempotent: subsequent calls are no-ops. */
void     pci_init(void);

/* Lookup helpers. Return non-zero on success and fill *out. */
int      pci_find_by_id   (uint16_t vendor, uint16_t device, struct pci_device *out);
int      pci_find_by_class(uint8_t class_code, uint8_t subclass, struct pci_device *out);

/* Number of devices found by the scan + indexed accessor. */
uint32_t pci_device_count(void);
int      pci_device_at(uint32_t idx, struct pci_device *out);

/* Capability list walk. Returns offset of cap with id `cap_id`, or 0 if absent. */
uint8_t  pci_find_capability(struct pci_addr a, uint8_t cap_id);

/* Enable bus mastering + MMIO/IO on a device (sets PCI_CMD bits). */
void     pci_enable(struct pci_device *d);

#endif
