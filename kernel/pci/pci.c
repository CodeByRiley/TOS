/* kernel/pci/pci.c — PCI/PCIe enumeration + config-space access.
 *
 * ACPI MCFG ranges use PCIe ECAM. A single 4 KiB virtual slot is remapped
 * to the requested function on demand, avoiding a permanent mapping for
 * every possible function. Segment 0 falls back to legacy 0xCF8/0xCFC
 * when no ECAM allocation covers the requested bus.
 *
 * Capability list walk lets vendor-specific drivers (virtio) find their
 * caps without re-walking the config header.
 */
#include "acpi/pci_mcfg.h"
#include "pci/pci.h"
#include "devices/io.h"
#include "memory/vmm.h"
#include "sync/spinlock.h"
#include "utilities/log.h"
#include <stdint.h>

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC
#define PCI_ECAM_VIRT 0xFFFFE00300000000ULL

#define MAX_PCI_DEVICES 64

static struct pci_device pci_table[MAX_PCI_DEVICES];
static uint32_t pci_count = 0;
static int      pci_scanned = 0;
static spinlock_t cfg_lock = SPINLOCK_INIT;
static uint64_t ecam_mapped_page = UINT64_MAX;

static uint32_t legacy_cfg_addr(struct pci_addr a, uint16_t off) {
    return (uint32_t)0x80000000u
         | ((uint32_t)a.bus << 16)
         | ((uint32_t)(a.dev & 0x1F) << 11)
         | ((uint32_t)(a.fn  & 0x07) << 8)
         | ((uint32_t)(off & 0xFC));
}

static int ecam_function_page(struct pci_addr a, uint64_t *page_phys) {
    struct pci_mcfg_range range;
    if (a.dev >= 32 || a.fn >= 8)
        return 0;
    if (!pci_mcfg_find(a.segment, a.bus, &range))
        return 0;

    *page_phys = range.base_phys
               + ((uint64_t)(a.bus - range.start_bus) << 20)
               + ((uint64_t)a.dev << 15)
               + ((uint64_t)a.fn << 12);
    return 1;
}

/* Caller holds cfg_lock. */
static int map_ecam_function(uint64_t page_phys) {
    if (ecam_mapped_page == page_phys)
        return 0;
    if (vmm_map(PCI_ECAM_VIRT, page_phys,
                VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT) != 0)
        return -1;
    ecam_mapped_page = page_phys;
    return 0;
}

/* Caller holds cfg_lock. */
static uint32_t cfg_read32_locked(struct pci_addr a, uint16_t off) {
    off &= 0xFFCu;

    uint64_t page_phys;
    if (ecam_function_page(a, &page_phys)) {
        if (map_ecam_function(page_phys) != 0)
            return 0xFFFFFFFFu;
        volatile uint32_t *reg =
            (volatile uint32_t*)(uintptr_t)(PCI_ECAM_VIRT + off);
        return *reg;
    }

    if (a.segment != 0 || off > 0xFC)
        return 0xFFFFFFFFu;
    outl(PCI_CFG_ADDR, legacy_cfg_addr(a, off));
    return inl(PCI_CFG_DATA);
}

/* Caller holds cfg_lock. */
static void cfg_write32_locked(struct pci_addr a, uint16_t off,
                               uint32_t value) {
    off &= 0xFFCu;

    uint64_t page_phys;
    if (ecam_function_page(a, &page_phys)) {
        if (map_ecam_function(page_phys) != 0)
            return;
        volatile uint32_t *reg =
            (volatile uint32_t*)(uintptr_t)(PCI_ECAM_VIRT + off);
        *reg = value;
        return;
    }

    if (a.segment != 0 || off > 0xFC)
        return;
    outl(PCI_CFG_ADDR, legacy_cfg_addr(a, off));
    outl(PCI_CFG_DATA, value);
}

uint32_t pci_cfg_read32(struct pci_addr a, uint16_t off) {
    if (off > 0xFFC)
        return 0xFFFFFFFFu;
    uint64_t flags = spin_lock_irqsave(&cfg_lock);
    uint32_t value = cfg_read32_locked(a, off);
    spin_unlock_irqrestore(&cfg_lock, flags);
    return value;
}

uint16_t pci_cfg_read16(struct pci_addr a, uint16_t off) {
    if (off > 0xFFE)
        return 0xFFFFu;
    uint64_t flags = spin_lock_irqsave(&cfg_lock);
    uint32_t value = cfg_read32_locked(a, off);
    spin_unlock_irqrestore(&cfg_lock, flags);
    return (uint16_t)(value >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(struct pci_addr a, uint16_t off) {
    if (off > 0xFFF)
        return 0xFFu;
    uint64_t flags = spin_lock_irqsave(&cfg_lock);
    uint32_t value = cfg_read32_locked(a, off);
    spin_unlock_irqrestore(&cfg_lock, flags);
    return (uint8_t)(value >> ((off & 3) * 8));
}

void pci_cfg_write32(struct pci_addr a, uint16_t off, uint32_t val) {
    if (off > 0xFFC)
        return;
    uint64_t flags = spin_lock_irqsave(&cfg_lock);
    cfg_write32_locked(a, off, val);
    spin_unlock_irqrestore(&cfg_lock, flags);
}

void pci_cfg_write16(struct pci_addr a, uint16_t off, uint16_t val) {
    if (off > 0xFFE)
        return;
    uint64_t flags = spin_lock_irqsave(&cfg_lock);
    uint32_t cur = cfg_read32_locked(a, off);
    int shift = (off & 2) * 8;
    cur &= ~(0xFFFFu << shift);
    cur |=  ((uint32_t)val << shift);
    cfg_write32_locked(a, off, cur);
    spin_unlock_irqrestore(&cfg_lock, flags);
}

void pci_cfg_write8(struct pci_addr a, uint16_t off, uint8_t val) {
    if (off > 0xFFF)
        return;
    uint64_t flags = spin_lock_irqsave(&cfg_lock);
    uint32_t cur = cfg_read32_locked(a, off);
    int shift = (off & 3) * 8;
    cur &= ~(0xFFu << shift);
    cur |=  ((uint32_t)val << shift);
    cfg_write32_locked(a, off, cur);
    spin_unlock_irqrestore(&cfg_lock, flags);
}

/* Read BAR pair (bar_off) and follow the size-discovery dance: save the
 * original value, write all-ones, read back the size mask, restore. Handles
 * 64-bit BARs by also touching bar_off+4. Returns the number of BAR slots
 * consumed (1 or 2). */
static int probe_bar(struct pci_addr a, int idx, struct pci_bar *out) {
    uint16_t off = (uint16_t)(PCI_CFG_BAR0 + idx * 4);
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
        // read original high DWORD and read low mask (assumed already read as mask_lo)
        uint32_t orig_hi = pci_cfg_read32(a, off + 4);

        // write all 1s to test writable address bits in the high DWORD
        pci_cfg_write32(a, off + 4, 0xFFFFFFFFu);
        uint32_t mask_hi = pci_cfg_read32(a, off + 4);

        // restore original high DWORD
        pci_cfg_write32(a, off + 4, orig_hi);

        // combine base address (clearing bottom 4 flag bits from orig/mask_lo)
        base = ((uint64_t)orig_hi << 32) | (orig & ~0x0Fu);

        // combine 64-bit mask and calculate size
        uint64_t full_mask = ((uint64_t)mask_hi << 32) | (uint64_t)(mask_lo & ~0x0Fu);

        // PCI size formula: (~mask) + 1
        // If full_mask is 0 (unimplemented/disabled BAR), size becomes 0
        size = full_mask ? (~full_mask + 1ULL) : 0ULL;
    }

    // if (is_64) {
    //     uint32_t orig_hi = pci_cfg_read32(a, off + 4);
    //     pci_cfg_write32(a, off + 4, 0xFFFFFFFFu);
    //     uint32_t mask_hi = pci_cfg_read32(a, off + 4);
    //     pci_cfg_write32(a, off + 4, orig_hi);
    //     base |= ((uint64_t)orig_hi << 32);
    //     /* Extend size mask into the high dword. If high mask is all-ones,
    //      * the region fits in 32 bits; if not, combine. */
    //     if (mask_hi != 0xFFFFFFFFu) {
    //         uint64_t full_mask = ((uint64_t)mask_hi << 32) | (mask_lo & 0xFFFFFFF0u);
    //         size = (~full_mask) + 1ULL;
    //     }
    // }

    out->base     = base;
    out->size     = size;
    out->is_io    = 0;
    out->is_64    = (uint8_t)is_64;
    out->prefetch = (orig & PCI_BAR_PREFETCH) ? 1 : 0;
    out->valid    = 1;
    return is_64 ? 2 : 1;
}

static void probe_rom(struct pci_addr a, struct pci_rom *out) {
    uint32_t original = pci_cfg_read32(a, PCI_CFG_ROM_ADDRESS);

    /* The enable bit is not part of the address mask and must remain clear
     * during the standard size-discovery write. */
    pci_cfg_write32(a, PCI_CFG_ROM_ADDRESS, 0xFFFFFFFEu);
    uint32_t mask = pci_cfg_read32(a, PCI_CFG_ROM_ADDRESS);
    pci_cfg_write32(a, PCI_CFG_ROM_ADDRESS, original);

    uint32_t address_mask = mask & PCI_ROM_ADDR_MASK;
    if (mask == 0xFFFFFFFFu || address_mask == 0) {
        out->base = 0;
        out->size = 0;
        out->enabled = 0;
        out->valid = 0;
        return;
    }

    out->base = original & PCI_ROM_ADDR_MASK;
    out->size = ((~(uint64_t)address_mask) & 0xFFFFFFFFULL) + 1;
    out->enabled = (original & PCI_ROM_ENABLE) ? 1 : 0;
    out->valid = 1;
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
    d->rom.valid = 0;
    if (d->header_type == 0) {
        int i = 0;
        while (i < 6) {
            int consumed = probe_bar(a, i, &d->bar[i]);
            i += consumed;
        }
        probe_rom(a, &d->rom);
    }
}

static void check_fn(struct pci_addr a) {
    uint16_t v = pci_cfg_read16(a, PCI_CFG_VENDOR_ID);
    if (v == 0xFFFF) return;
    describe_fn(a);
}

static void check_dev(uint16_t segment, uint8_t bus, uint8_t dev) {
    struct pci_addr a = {
        .bus = bus,
        .dev = dev,
        .fn = 0,
        .segment = segment,
    };
    uint16_t v = pci_cfg_read16(a, PCI_CFG_VENDOR_ID);
    if (v == 0xFFFF) return;
    check_fn(a);

    /* Multifunction bit (header type 0x80): scan fns 1..7. */
    uint8_t hdr = pci_cfg_read8(a, PCI_CFG_HEADER_TYPE);
    if (hdr & 0x80) {
        for (uint8_t fn = 1; fn < 8; fn++) {
            struct pci_addr af = {
                .bus = bus,
                .dev = dev,
                .fn = fn,
                .segment = segment,
            };
            check_fn(af);
        }
    }
}

static int scan_bus_range(uint16_t segment, uint8_t start_bus,
                          uint8_t end_bus) {
    for (uint16_t bus = start_bus; bus <= end_bus; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            check_dev(segment, (uint8_t)bus, dev);
            if (pci_count >= MAX_PCI_DEVICES)
                return -1;
        }
    }
    return 0;
}

void pci_init(void) {
    if (pci_scanned) return;
    pci_scanned = 1;
    pci_count = 0;

    uint32_t ecam_ranges = pci_mcfg_range_count();
    if (ecam_ranges) {
        log_write("PCI: using ACPI MCFG ECAM", KERNEL, LOG_INFO);
        for (uint32_t i = 0; i < ecam_ranges; i++) {
            struct pci_mcfg_range range;
            if (!pci_mcfg_range_at(i, &range))
                continue;
            if (scan_bus_range(range.segment, range.start_bus,
                               range.end_bus) != 0)
                break;
        }
    } else {
        log_write("PCI: using legacy config IO", KERNEL, LOG_INFO);
        scan_bus_range(0, 0, 255);
    }

    log_write_hex("PCI: devices found =", pci_count, KERNEL, LOG_INFO);

    for (uint32_t i = 0; i < pci_count; i++) {
        struct pci_device *d = &pci_table[i];

        // Combine Vendor and Device into a single 32-bit hex number for printing
        uint32_t vendor_device = ((uint32_t)d->vendor << 16) | d->device;

        // Combine Class, Subclass, and Prog IF
        uint32_t class_info = ((uint32_t)d->class_code << 16) |
                              ((uint32_t)d->subclass << 8) |
                              d->prog_if;

        log_write("------------------------", KERNEL, LOG_INFO);
        log_write_hex("PCI: Vendor:Device =", vendor_device, KERNEL, LOG_INFO);
        log_write_hex("PCI: Class:Sub:Prog =", class_info, KERNEL, LOG_INFO);

        // Log Bus:Dev:Fn
        uint32_t bdf = ((uint32_t)d->addr.bus << 16) |
                       ((uint32_t)d->addr.dev << 8) |
                       d->addr.fn;
        log_write_hex("PCI: Bus:Dev:Fn    =", bdf, KERNEL, LOG_INFO);

        // Log IRQ info
        uint32_t irq_info = ((uint32_t)d->int_pin << 8) | d->int_line;
        log_write_hex("PCI: IntPin:IntLine =", irq_info, KERNEL, LOG_INFO);

        // Log the first valid BAR (usually MMIO or I/O base)
        for (int b = 0; b < 6; b++) {
            if (d->bar[b].valid) {
                uint32_t bar_info = (d->bar[b].is_io << 24) | (uint32_t)d->bar[b].size;
                log_write_hex("PCI: BAR (IO/Size) =", bar_info, KERNEL, LOG_INFO);
                log_write_hex("PCI: BAR Base      =", d->bar[b].base, KERNEL, LOG_INFO);
                break; // Just print the first one to avoid log spam
            }
        }
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

uint16_t pci_find_ext_capability(struct pci_addr a, uint16_t cap_id) {
    uint16_t off = 0x100;
    int hops = 0;

    while (off && off <= 0xFFC && hops++ < 256) {
        uint32_t header = pci_cfg_read32(a, off);
        if (header == 0 || header == 0xFFFFFFFFu)
            return 0;
        if ((header & 0xFFFFu) == cap_id)
            return off;

        uint16_t next = (uint16_t)((header >> 20) & 0xFFFu);
        if (next < 0x100 || (next & 3) || next == off)
            return 0;
        off = next;
    }
    return 0;
}

int pci_enable_memory(struct pci_device *d) {
    if (!d)
        return -1;

    uint16_t cmd = pci_cfg_read16(d->addr, PCI_CFG_COMMAND);
    if (cmd & PCI_CMD_MEM)
        return 0;

    pci_cfg_write16(d->addr, PCI_CFG_COMMAND, cmd | PCI_CMD_MEM);
    return (pci_cfg_read16(d->addr, PCI_CFG_COMMAND) & PCI_CMD_MEM) ? 0 : -1;
}

void pci_enable(struct pci_device *d) {
    uint16_t cmd = pci_cfg_read16(d->addr, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEM | PCI_CMD_BUS_MASTER;
    /* Keep INTx enabled (clear INT_DISABLE) for legacy IRQ delivery. */
    cmd &= ~PCI_CMD_INT_DISABLE;
    pci_cfg_write16(d->addr, PCI_CFG_COMMAND, cmd);
}
