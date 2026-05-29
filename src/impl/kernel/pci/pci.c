#include "pci/pci.h"
#include "devices/io.h"
#include "utilities/log.h"
#include <stdint.h>

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

#define MAX_PCI_DEVICES 64

static struct pci_device pci_table[MAX_PCI_DEVICES];
static uint32_t pci_count = 0;
static int      pci_scanned = 0;

static uint32_t cfg_addr(struct pci_addr a, uint8_t off) {
    /* The legacy CF8/CFC mechanism only exposes the first 256 bytes of config
     * space; MCFG (PCIe ECAM) is required for extended config but virtio-pci
     * fits within the first 64 bytes for our purposes. */
    return (uint32_t)0x80000000u
         | ((uint32_t)a.bus << 16)
         | ((uint32_t)(a.dev & 0x1F) << 11)
         | ((uint32_t)(a.fn  & 0x07) << 8)
         | ((uint32_t)(off & 0xFC));
}

uint32_t pci_cfg_read32(struct pci_addr a, uint8_t off) {
    outl(PCI_CFG_ADDR, cfg_addr(a, off));
    return inl(PCI_CFG_DATA);
}

uint16_t pci_cfg_read16(struct pci_addr a, uint8_t off) {
    uint32_t v = pci_cfg_read32(a, off & 0xFC);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(struct pci_addr a, uint8_t off) {
    uint32_t v = pci_cfg_read32(a, off & 0xFC);
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_cfg_write32(struct pci_addr a, uint8_t off, uint32_t val) {
    outl(PCI_CFG_ADDR, cfg_addr(a, off));
    outl(PCI_CFG_DATA, val);
}

void pci_cfg_write16(struct pci_addr a, uint8_t off, uint16_t val) {
    uint32_t cur = pci_cfg_read32(a, off & 0xFC);
    int shift = (off & 2) * 8;
    cur &= ~(0xFFFFu << shift);
    cur |=  ((uint32_t)val << shift);
    pci_cfg_write32(a, off & 0xFC, cur);
}

void pci_cfg_write8(struct pci_addr a, uint8_t off, uint8_t val) {
    uint32_t cur = pci_cfg_read32(a, off & 0xFC);
    int shift = (off & 3) * 8;
    cur &= ~(0xFFu << shift);
    cur |=  ((uint32_t)val << shift);
    pci_cfg_write32(a, off & 0xFC, cur);
}

/* Read BAR pair (bar_off) and follow the size-discovery dance: save the
 * original value, write all-ones, read back the size mask, restore. Handles
 * 64-bit BARs by also touching bar_off+4. Returns the number of BAR slots
 * consumed (1 or 2). */
static int probe_bar(struct pci_addr a, int idx, struct pci_bar *out) {
    uint8_t off = (uint8_t)(PCI_CFG_BAR0 + idx * 4);
    uint32_t orig = pci_cfg_read32(a, off);
    if (orig == 0) {
        out->valid = 0;
        return 1;
    }

    if (orig & PCI_BAR_IO) {
        /* I/O BAR (port space). */
        pci_cfg_write32(a, off, 0xFFFFFFFFu);
        uint32_t mask = pci_cfg_read32(a, off);
        pci_cfg_write32(a, off, orig);
        out->base     = orig & ~0x3ULL;
        out->size     = (~(mask & ~0x3u)) + 1u;
        out->is_io    = 1;
        out->is_64    = 0;
        out->prefetch = 0;
        out->valid    = 1;
        return 1;
    }

    /* MMIO BAR. */
    int is_64 = (orig & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64;
    pci_cfg_write32(a, off, 0xFFFFFFFFu);
    uint32_t mask_lo = pci_cfg_read32(a, off);
    pci_cfg_write32(a, off, orig);

    uint64_t base = orig & 0xFFFFFFF0u;
    uint64_t size = (uint64_t)(~(mask_lo & 0xFFFFFFF0u)) + 1u;

    if (is_64) {
        uint32_t orig_hi = pci_cfg_read32(a, off + 4);
        pci_cfg_write32(a, off + 4, 0xFFFFFFFFu);
        uint32_t mask_hi = pci_cfg_read32(a, off + 4);
        pci_cfg_write32(a, off + 4, orig_hi);
        base |= ((uint64_t)orig_hi << 32);
        /* Extend size mask into the high dword. If high mask is all-ones,
         * the region fits in 32 bits; if not, combine. */
        if (mask_hi != 0xFFFFFFFFu) {
            uint64_t full_mask = ((uint64_t)mask_hi << 32) | (mask_lo & 0xFFFFFFF0u);
            size = (~full_mask) + 1ULL;
        }
    }

    out->base     = base;
    out->size     = size;
    out->is_io    = 0;
    out->is_64    = (uint8_t)is_64;
    out->prefetch = (orig & PCI_BAR_PREFETCH) ? 1 : 0;
    out->valid    = 1;
    return is_64 ? 2 : 1;
}

static void describe_fn(struct pci_addr a) {
    if (pci_count >= MAX_PCI_DEVICES) return;
    uint16_t vendor = pci_cfg_read16(a, PCI_CFG_VENDOR_ID);
    if (vendor == 0xFFFF) return;

    struct pci_device *d = &pci_table[pci_count++];
    d->addr          = a;
    d->vendor        = vendor;
    d->device        = pci_cfg_read16(a, PCI_CFG_DEVICE_ID);
    d->revision      = pci_cfg_read8 (a, PCI_CFG_REVISION);
    d->prog_if       = pci_cfg_read8 (a, PCI_CFG_PROG_IF);
    d->subclass      = pci_cfg_read8 (a, PCI_CFG_SUBCLASS);
    d->class_code    = pci_cfg_read8 (a, PCI_CFG_CLASS);
    d->header_type   = pci_cfg_read8 (a, PCI_CFG_HEADER_TYPE) & 0x7F;
    d->subsys_vendor = pci_cfg_read16(a, PCI_CFG_SUBSYS_VENDOR);
    d->subsys_id     = pci_cfg_read16(a, PCI_CFG_SUBSYS_ID);
    d->int_line      = pci_cfg_read8 (a, PCI_CFG_INT_LINE);
    d->int_pin       = pci_cfg_read8 (a, PCI_CFG_INT_PIN);

    uint16_t status  = pci_cfg_read16(a, PCI_CFG_STATUS);
    d->cap_ptr = (status & PCI_STATUS_CAP_LIST)
               ? (pci_cfg_read8(a, PCI_CFG_CAP_PTR) & 0xFC)
               : 0;

    /* Type 1 (PCI-to-PCI bridge) has BARs at 0/1 only — skip BARs for
     * non-zero header types to avoid garbage probing the bridge IO window. */
    for (int i = 0; i < 6; i++) {
        d->bar[i].valid = 0;
    }
    if (d->header_type == 0) {
        int i = 0;
        while (i < 6) {
            int consumed = probe_bar(a, i, &d->bar[i]);
            i += consumed;
        }
    }
}

static void check_fn(struct pci_addr a) {
    uint16_t v = pci_cfg_read16(a, PCI_CFG_VENDOR_ID);
    if (v == 0xFFFF) return;
    describe_fn(a);
}

static void check_dev(uint8_t bus, uint8_t dev) {
    struct pci_addr a = { bus, dev, 0 };
    uint16_t v = pci_cfg_read16(a, PCI_CFG_VENDOR_ID);
    if (v == 0xFFFF) return;
    check_fn(a);

    /* Multifunction bit (header type 0x80): scan fns 1..7. */
    uint8_t hdr = pci_cfg_read8(a, PCI_CFG_HEADER_TYPE);
    if (hdr & 0x80) {
        for (uint8_t fn = 1; fn < 8; fn++) {
            struct pci_addr af = { bus, dev, fn };
            check_fn(af);
        }
    }
}

void pci_init(void) {
    if (pci_scanned) return;
    pci_scanned = 1;
    pci_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            check_dev((uint8_t)bus, dev);
            if (pci_count >= MAX_PCI_DEVICES) goto done;
        }
    }
done:
    log_write_hex("PCI: devices found =", pci_count, KERNEL, LOG_INFO);
    for (uint32_t i = 0; i < pci_count; i++) {
        struct pci_device *d = &pci_table[i];
        uint32_t tag = ((uint32_t)d->vendor << 16) | d->device;
        log_write_hex("PCI: vendor:device  =", tag, KERNEL, LOG_INFO);
    }
}

int pci_find_by_id(uint16_t vendor, uint16_t device, struct pci_device *out) {
    for (uint32_t i = 0; i < pci_count; i++) {
        if (pci_table[i].vendor == vendor && pci_table[i].device == device) {
            *out = pci_table[i];
            return 1;
        }
    }
    return 0;
}

int pci_find_by_class(uint8_t cls, uint8_t sub, struct pci_device *out) {
    for (uint32_t i = 0; i < pci_count; i++) {
        if (pci_table[i].class_code == cls && pci_table[i].subclass == sub) {
            *out = pci_table[i];
            return 1;
        }
    }
    return 0;
}

uint32_t pci_device_count(void) { return pci_count; }

int pci_device_at(uint32_t idx, struct pci_device *out) {
    if (idx >= pci_count) return 0;
    *out = pci_table[idx];
    return 1;
}

uint8_t pci_find_capability(struct pci_addr a, uint8_t cap_id) {
    uint16_t status = pci_cfg_read16(a, PCI_CFG_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;

    uint8_t off = pci_cfg_read8(a, PCI_CFG_CAP_PTR) & 0xFC;
    int hops = 0;
    while (off && hops++ < 48) {
        uint8_t id   = pci_cfg_read8(a, off);
        if (id == cap_id) return off;
        uint8_t next = pci_cfg_read8(a, off + 1) & 0xFC;
        if (next == off) break;
        off = next;
    }
    return 0;
}

void pci_enable(struct pci_device *d) {
    uint16_t cmd = pci_cfg_read16(d->addr, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEM | PCI_CMD_BUS_MASTER;
    /* Keep INTx enabled (clear INT_DISABLE) for legacy IRQ delivery. */
    cmd &= ~PCI_CMD_INT_DISABLE;
    pci_cfg_write16(d->addr, PCI_CFG_COMMAND, cmd);
}
