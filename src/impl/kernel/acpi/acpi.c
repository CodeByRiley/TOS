/* src/impl/kernel/acpi/acpi.c — ACPI discovery + MADT parser.
 *
 * Two RSDP sources, in order of preference:
 *   1. Multiboot2 ACPI_NEW (XSDP) or ACPI_OLD (RSDP) tag
 *   2. Legacy BIOS scan: EBDA pointer + the 0xE0000-0xFFFFF window
 *
 * For SMP we only care about the LAPIC MMIO base and the list of enabled
 * processors. Everything else in MADT is ignored.
 *
 * Boot identity-maps only the first 1 GiB (main.asm). Firmware likes to
 * park ACPI tables near the top of RAM, so acpi_map() lazily pages in
 * any 4 KiB window outside the boot identity range before we touch it.
 */
#include "acpi/acpi.h"
#include "boot/multiboot2.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

static uint64_t lapic_phys = 0;
static uint8_t  cpu_ids[ACPI_MAX_CPUS];
static int      cpu_count = 0;

/* Boot identity-maps only the first 1 GiB (main.asm), but firmware likes to
 * park ACPI tables near the top of RAM — on QEMU with 2 GiB that's ~0x7FFE0000,
 * which faults if we just dereference it. acpi_map ensures every 4 KiB page
 * touched by `[phys, phys+len)` has an identity mapping in the current
 * kernel PML4. Idempotent: vmm_map silently no-ops if the page already maps
 * to the same frame.
 *
 * We avoid using vmm_map for anything inside the existing 1 GiB huge-page
 * region — walk_or_create returns -1 when it hits a 2 MiB present mapping
 * (correctly: it can't sub-divide a huge page without breaking it). */
static void *acpi_map(uint64_t phys, size_t len) {
    if (phys + len <= 0x40000000ULL) return (void*)phys;   /* in boot identity */
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end   = (phys + len + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000) {
        if (p < 0x40000000ULL) continue;
        vmm_map(p, p, VMM_PRESENT | VMM_WRITE);
    }
    return (void*)phys;
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
        if (checksum(r2, r2->length) != 0)       return 0;
    }
    return r;
}

/* Scan a 64K-aligned 16-byte-stepped region for the RSDP signature. */
static const struct acpi_rsdp_v1 *scan_for_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += 16) {
        const struct acpi_rsdp_v1 *r = validate_rsdp((const uint8_t*)addr);
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
    uint16_t ebda_seg = *(uint16_t*)0x40E;
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
    /* Map header first to learn the table's length, then re-map the full
     * extent. Two-step because the length field is inside the header itself. */
    acpi_map(phys, sizeof(struct acpi_sdt_header));
    const struct acpi_sdt_header *h = (const struct acpi_sdt_header*)phys;
    acpi_map(phys, h->length);
    return h;
}

int acpi_init(uint64_t mb2_addr) {
    const struct acpi_rsdp_v1 *rsdp = find_rsdp(mb2_addr);
    if (!rsdp) {
        log_write("ACPI: no RSDP found", KERNEL, LOG_ERROR);
        return -1;
    }
    log_write_hex("ACPI: RSDP revision  =", rsdp->revision, KERNEL, LOG_INFO);

    /* Walk RSDT (v1) or XSDT (v2) looking for "APIC" (= MADT). */
    const struct acpi_madt *madt = 0;
    if (rsdp->revision >= 2) {
        const struct acpi_rsdp_v2 *r2 = (const struct acpi_rsdp_v2*)rsdp;
        const struct acpi_sdt_header *xsdt = map_sdt(r2->xsdt_phys);
        if (!sig_eq(xsdt->signature, ACPI_SIG_XSDT, 4)) return -1;
        int n = (xsdt->length - sizeof(*xsdt)) / 8;
        const uint64_t *ents = (const uint64_t*)((const uint8_t*)xsdt + sizeof(*xsdt));
        for (int i = 0; i < n; i++) {
            const struct acpi_sdt_header *h = map_sdt(ents[i]);
            if (sig_eq(h->signature, ACPI_SIG_APIC, 4)) { madt = (const struct acpi_madt*)h; break; }
        }
    } else {
        const struct acpi_sdt_header *rsdt = map_sdt((uint64_t)rsdp->rsdt_phys);
        if (!sig_eq(rsdt->signature, ACPI_SIG_RSDT, 4)) return -1;
        int n = (rsdt->length - sizeof(*rsdt)) / 4;
        const uint32_t *ents = (const uint32_t*)((const uint8_t*)rsdt + sizeof(*rsdt));
        for (int i = 0; i < n; i++) {
            const struct acpi_sdt_header *h = map_sdt((uint64_t)ents[i]);
            if (sig_eq(h->signature, ACPI_SIG_APIC, 4)) { madt = (const struct acpi_madt*)h; break; }
        }
    }

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
