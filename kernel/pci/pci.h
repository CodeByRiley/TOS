/* kernel/pci/pci.h , PCI/PCIe enumeration + config-space access.
 *
 * Brute-force scan on init (256 busses x 32 devs x 8 fns). Devices land
 * in an internal table; lookup helpers find by vendor/device or class/
 * subclass. BAR layout is decoded eagerly so MMIO mappings just need a
 * `bar[n].base` + `.size` lookup.
 *
 * ACPI MCFG ECAM is used when available, exposing the full 4 KiB PCIe
 * configuration function. Segment 0 falls back to legacy 0xCF8/0xCFC.
 *
 * Implementation: kernel/pci/pci.c.
 */
#ifndef PCI_H
#define PCI_H

#include <utilities/types.h>
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
#define PCI_CFG_ROM_ADDRESS     0x30
#define PCI_CFG_CAP_PTR         0x34
#define PCI_CFG_INT_LINE        0x3C
#define PCI_CFG_INT_PIN         0x3D

/* Command Register (offset 0x04) */
//
// Bits  | Name              | Description
// 15-11 | Reserved          |
// 10    | Interrupt Disable | 1 = Device may not assert INTx
// 9     | Fast B2B Enable   |
// 8     | SERR# Enable      |
// 7     | Reserved          |
// 6     | Parity Error Resp |
// 5     | VGA Palette Snoop |
// 4     | Mem Write & Inval |
// 3     | Special Cycles    |
// 2     | Bus Master        | 1 = Device may initiate DMA
// 1     | Memory Space      | 1 = MMIO BARs decode
// 0     | I/O Space         | 1 = I/O BARs decode
//
// Space bits gate decoding entirely: with I/O Space clear, every inb/outb to
// an I/O BAR reads 0xFF and drops writes, silently. Bus Master gates DMA the
// same way , a device with descriptors queued simply never fetches them.
#define PCI_CMD_IO              (1u << 0)
#define PCI_CMD_MEM             (1u << 1)
#define PCI_CMD_BUS_MASTER      (1u << 2)
#define PCI_CMD_INT_DISABLE     (1u << 10)

/* Status Register (offset 0x06). Bit 4 is the only one we consume: it says
 * whether offset 0x34 holds a valid capability-list pointer. */
#define PCI_STATUS_CAP_LIST     (1u << 4)

/* Base Address Register (offsets 0x10-0x24)
 *
 * MMIO BAR:                       I/O BAR:
 *   31-4 | Base address             31-2 | Base address
 *   3    | Prefetchable             1    | Reserved
 *   2-1  | Type: 0=32b, 2=64b       0    | Always 1
 *   0    | Always 0
 *
 * A 64-bit MMIO BAR consumes the following slot as its high dword, so BAR
 * indices are not always contiguous. Size is discovered by writing all ones
 * and reading back the mask , which is why a BAR must be saved and restored,
 * and why decoding should be off while probing. */
#define PCI_BAR_IO              (1u << 0)
#define PCI_BAR_TYPE_MASK       (3u << 1)
#define PCI_BAR_TYPE_32         (0u << 1)
#define PCI_BAR_TYPE_64         (2u << 1)
#define PCI_BAR_PREFETCH        (1u << 3)

#define PCI_ROM_ENABLE          (1u << 0)
#define PCI_ROM_ADDR_MASK       0xFFFFF800u

struct pci_addr {
    u8 bus;
    u8 dev;
    u8 fn;
    u16 segment;
};

/* Decoded BAR. `base` and `size` are post-decode (mask bits stripped). */
struct pci_bar {
    u64 base;
    u64 size;
    u8  is_io;     /* 1 = I/O port BAR, 0 = MMIO */
    u8  is_64;     /* 1 = 64-bit MMIO BAR (consumes two slots) */
    u8  prefetch;
    u8  valid;     /* 1 if populated */
};

struct pci_rom {
    u64 base;
    u64 size;
    u8  enabled;
    u8  valid;
};

struct pci_device {
    struct pci_addr addr;
    u16        vendor;
    u16        device;
    u16        subsys_vendor;
    u16        subsys_id;
    u8         class_code;
    u8         subclass;
    u8         prog_if;
    u8         revision;
    u8         header_type;
    u8         int_line;
    u8         int_pin;
    u8         cap_ptr;       /* offset of first capability, 0 if none */
    struct pci_bar  bar[6];
    struct pci_rom  rom;
};

/* Raw config-space accessors. */
u32 pci_read32(struct pci_addr a, u16 off);
u16 pci_read16(struct pci_addr a, u16 off);
u8  pci_read8 (struct pci_addr a, u16 off);
void     pci_write32(struct pci_addr a, u16 off, u32 val);
void     pci_write16(struct pci_addr a, u16 off, u16 val);
void     pci_write8 (struct pci_addr a, u16 off, u8  val);

/* Brute-force scan all 256 busses * 32 devs * 8 fns. Populates the
 * internal device table. Idempotent: re-entry is a no-op. */
void     pci_init(void);

/* Lookup helpers. Return non-zero on success and fill *out. */
int      pci_find_by_id   (u16 vendor, u16 device, struct pci_device *out);
int      pci_find_by_class(u8 class_code, u8 subclass, struct pci_device *out);

/* Indexed accessor over the scan results. */
u32 pci_device_count(void);
int      pci_device_at(u32 idx, struct pci_device *out);

/* Walk the device's capability list looking for `cap_id`. Returns offset
 * or 0 if absent. */
u8  pci_find_capability(struct pci_addr a, u8 cap_id);

/* Walk PCIe's extended capability chain at offsets 0x100..0xFFF. */
u16 pci_find_ext_capability(struct pci_addr a, u16 cap_id);

/* Enable MMIO decoding without enabling DMA or changing interrupt state. */
int      pci_enable_memory(struct pci_device *d);

/* Set the PCI_CMD bits for bus mastering + MMIO/IO on a device. */
void     pci_enable(struct pci_device *d);

#endif
