/* src/impl/kernel/acpi/acpi.c — ACPI discovery + SDT/MADT parsing.
 *
 * Two RSDP sources, in order of preference:
 *   1. Multiboot2 ACPI_NEW (XSDP) or ACPI_OLD (RSDP) tag
 *   2. Legacy BIOS scan: EBDA pointer + the 0xE0000-0xFFFFF window
 *
 * RSDT/XSDT discovery is retained as a table directory so other subsystems
 * can request checksum-valid SDTs by signature. MADT supplies SMP topology;
 * MCFG is handed to the PCI ECAM parser.
 *
 * ACPI physical addresses are accessed through the kernel HHDM, which PMM
 * extends across the physical memory-map endpoints before ACPI starts.
 */
#include "acpi/acpi.h"
#include "acpi/pci_mcfg.h"
#include "boot/multiboot2.h"
#include "memory/hhdm.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

static uint64_t lapic_phys = 0;
static uint8_t  cpu_ids[ACPI_MAX_CPUS];
static int      cpu_count = 0;
static const struct acpi_sdt_header *root_sdt;
static uint8_t root_entry_size;

#define ACPI_MAX_TABLE_LENGTH (16u * 1024u * 1024u)

/* Return an HHDM pointer for an ACPI physical range. */
static void *acpi_map(uint64_t phys, size_t len) {
    (void)len;
    return phys_to_virt(phys);
}

static uint8_t checksum(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t*)p;
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += b[i];
    return s;
}

static int sig_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static const struct acpi_rsdp_v1 *validate_rsdp(const uint8_t *p) {
    const struct acpi_rsdp_v1 *r = (const struct acpi_rsdp_v1*)p;
    if (!sig_eq(r->signature, ACPI_SIG_RSDP, 8)) return 0;
    if (checksum(r, sizeof(*r)) != 0)            return 0;
    if (r->revision >= 2) {
        const struct acpi_rsdp_v2 *r2 = (const struct acpi_rsdp_v2*)p;
        if (r2->length < sizeof(*r2) || r2->length > 4096) return 0;
        if (checksum(r2, r2->length) != 0)       return 0;
    }
    return r;
}

/* Scan a 64K-aligned 16-byte-stepped region for the RSDP signature. */
static const struct acpi_rsdp_v1 *scan_for_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += 16) {
        const struct acpi_rsdp_v1 *r =
            validate_rsdp(phys_to_virt(addr));
        if (r) return r;
    }
    return 0;
}

static const struct acpi_rsdp_v1 *find_rsdp(uint64_t mb2_addr) {
    /* 1. Multiboot tag (preferred — explicit, validated). */
    struct MB2_TAG_ACPI *t = (struct MB2_TAG_ACPI*)
        mb2_find_tag(mb2_addr, MULTIBOOT_TAG_ACPI_NEW);
    if (!t) t = (struct MB2_TAG_ACPI*)
        mb2_find_tag(mb2_addr, MULTIBOOT_TAG_ACPI_OLD);
    if (t) {
        const struct acpi_rsdp_v1 *r = validate_rsdp(t->rsdp);
        if (r) return r;
    }

    /* 2. EBDA pointer at 0x40E (segment) -> first 1 KiB of EBDA. */
    uint16_t ebda_seg = *(uint16_t*)phys_to_virt(0x40E);
    if (ebda_seg) {
        uint64_t ebda = (uint64_t)ebda_seg << 4;
        const struct acpi_rsdp_v1 *r = scan_for_rsdp(ebda, ebda + 0x400);
        if (r) return r;
    }

    /* 3. BIOS extended region 0xE0000-0xFFFFF. */
    return scan_for_rsdp(0xE0000, 0x100000);
}

static int parse_madt(const struct acpi_madt *madt) {
    if (!sig_eq(madt->h.signature, ACPI_SIG_APIC, 4)) return -1;
    if (checksum(madt, madt->h.length) != 0)          return -1;

    lapic_phys = madt->lapic_phys;
    cpu_count  = 0;

    const uint8_t *p   = (const uint8_t*)madt + sizeof(*madt);
    const uint8_t *end = (const uint8_t*)madt + madt->h.length;
    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2 || p + len > end) break;
        if (type == MADT_TYPE_LOCAL_APIC && len >= sizeof(struct madt_entry_local_apic)) {
            const struct madt_entry_local_apic *e = (const struct madt_entry_local_apic*)p;
            int usable = (e->flags & MADT_LAPIC_FLAG_ENABLED) ||
                         (e->flags & MADT_LAPIC_FLAG_ONLINE_CAPABLE);
            if (usable && cpu_count < ACPI_MAX_CPUS) {
                cpu_ids[cpu_count++] = e->apic_id;
            }
        } else if (type == MADT_TYPE_LOCAL_APIC_OVR && len >= 12) {
            /* 64-bit LAPIC base override — wins over the 32-bit MADT field. */
            lapic_phys = *(const uint64_t*)(p + 4);
        }
        p += len;
    }
    return 0;
}

static const struct acpi_sdt_header *map_sdt(uint64_t phys) {
    if (!phys) return 0;

    /* Map header first to learn the table's length, then re-map the full
     * extent. Two-step because the length field is inside the header itself. */
    const struct acpi_sdt_header *h =
        acpi_map(phys, sizeof(struct acpi_sdt_header));
    if (h->length < sizeof(*h) || h->length > ACPI_MAX_TABLE_LENGTH)
        return 0;
    return acpi_map(phys, h->length);
}

static int set_root_sdt(uint64_t phys, const char signature[4],
                        uint8_t entry_size) {
    const struct acpi_sdt_header *root = map_sdt(phys);
    if (!root || !sig_eq(root->signature, signature, 4))
        return -1;
    if (checksum(root, root->length) != 0)
        return -1;
    if ((root->length - sizeof(*root)) % entry_size != 0)
        return -1;

    root_sdt = root;
    root_entry_size = entry_size;
    return 0;
}

const struct acpi_sdt_header *acpi_find_table(const char signature[4],
                                              uint32_t index) {
    if (!root_sdt || !root_entry_size)
        return 0;

    uint32_t count =
        (root_sdt->length - sizeof(*root_sdt)) / root_entry_size;
    const uint8_t *entries = (const uint8_t*)root_sdt + sizeof(*root_sdt);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t phys;
        if (root_entry_size == 8)
            phys = ((const uint64_t*)entries)[i];
        else
            phys = ((const uint32_t*)entries)[i];

        const struct acpi_sdt_header *table = map_sdt(phys);
        if (!table || checksum(table, table->length) != 0)
            continue;
        if (!sig_eq(table->signature, signature, 4))
            continue;
        if (index-- == 0)
            return table;
    }

    return 0;
}

int acpi_init(uint64_t mb2_addr) {
    root_sdt = 0;
    root_entry_size = 0;
    lapic_phys = 0;
    cpu_count = 0;
    pci_mcfg_init(0);

    const struct acpi_rsdp_v1 *rsdp = find_rsdp(mb2_addr);
    if (!rsdp) {
        log_write("ACPI: no RSDP found", KERNEL, LOG_ERROR);
        return -1;
    }
    log_write_hex("ACPI: RSDP revision  =", rsdp->revision, KERNEL, LOG_INFO);

    /* Prefer XSDT when ACPI 2.0+ supplies one; otherwise use RSDT. */
    int root_ok = -1;
    if (rsdp->revision >= 2) {
        const struct acpi_rsdp_v2 *r2 = (const struct acpi_rsdp_v2*)rsdp;
        if (r2->xsdt_phys)
            root_ok = set_root_sdt(r2->xsdt_phys, ACPI_SIG_XSDT, 8);
    }
    if (root_ok != 0)
        root_ok = set_root_sdt((uint64_t)rsdp->rsdt_phys,
                               ACPI_SIG_RSDT, 4);
    if (root_ok != 0) {
        log_write("ACPI: invalid RSDT/XSDT", KERNEL, LOG_ERROR);
        return -1;
    }

    const struct acpi_sdt_header *mcfg = acpi_find_table(ACPI_SIG_MCFG, 0);
    if (mcfg) {
        if (pci_mcfg_init(mcfg) != 0)
            log_write("ACPI: MCFG parse failed", KERNEL, LOG_WARN);
    } else {
        log_write("ACPI: no MCFG, PCI will use legacy config IO",
                  KERNEL, LOG_INFO);
    }

    const struct acpi_madt *madt =
        (const struct acpi_madt*)acpi_find_table(ACPI_SIG_APIC, 0);
    if (!madt) {
        log_write("ACPI: no MADT (APIC table) found", KERNEL, LOG_ERROR);
        return -1;
    }
    if (parse_madt(madt) != 0) {
        log_write("ACPI: MADT parse failed", KERNEL, LOG_ERROR);
        return -1;
    }
    log_write_hex("ACPI: LAPIC base phys =", lapic_phys, KERNEL, LOG_INFO);
    log_write_hex("ACPI: cpu count       =", (uint64_t)cpu_count, KERNEL, LOG_INFO);
    return 0;
}

uint64_t acpi_lapic_phys(void)        { return lapic_phys; }
int      acpi_cpu_count(void)         { return cpu_count;  }
uint8_t  acpi_cpu_apic_id(int idx)    {
    if (idx < 0 || idx >= cpu_count) return 0xFF;
    return cpu_ids[idx];
}
