/* kernel/acpi/acpi.h — ACPI table parsing surface.
 *
 * RSDP discovery establishes a reusable RSDT/XSDT table directory.
 * Subsystems can then find validated SDTs by signature. ACPI itself parses
 * MADT for SMP; PCI consumes MCFG through acpi/pci_mcfg.h.
 *
 * Implementation: kernel/acpi/acpi.c.
 */
#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* Signatures used to locate the four tables we care about. RSDP's
 * trailing space is part of the signature per spec. */
#define ACPI_SIG_RSDP "RSD PTR "
#define ACPI_SIG_RSDT "RSDT"
#define ACPI_SIG_XSDT "XSDT"
#define ACPI_SIG_APIC "APIC"            /* MADT signature */

/* ACPI 1.0 Root System Description Pointer (20 bytes). */
struct __attribute__((packed)) acpi_rsdp_v1 {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;                  /* 0 = v1.0, 2 = v2.0+ */
    uint32_t rsdt_phys;
};

/* ACPI 2.0+ RSDP (36 bytes; embeds v1 followed by extended fields). */
struct __attribute__((packed)) acpi_rsdp_v2 {
    struct acpi_rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
};

/* Header common to every SDT. `length` covers the header + variable body. */
struct __attribute__((packed)) acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/* Multiple APIC Description Table — enumerates LAPICs / IOAPICs. */
struct __attribute__((packed)) acpi_madt {
    struct acpi_sdt_header h;
    uint32_t lapic_phys;                /* default LAPIC MMIO base */
    uint32_t flags;                     /* bit 0: legacy PIC present */
    /* variable-length entries follow */
};

/* MADT entry types. We only consume LOCAL_APIC; the others are recognised
 * by the walker but skipped. */
#define MADT_TYPE_LOCAL_APIC        0
#define MADT_TYPE_IO_APIC           1
#define MADT_TYPE_INT_OVERRIDE      2
#define MADT_TYPE_LOCAL_APIC_OVR    5
#define MADT_TYPE_LOCAL_X2APIC      9

/* One CPU's LAPIC entry inside MADT. */
struct __attribute__((packed)) madt_entry_local_apic {
    uint8_t  type;                      /* = 0 */
    uint8_t  length;                    /* = 8 */
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;                     /* bit 0: enabled, bit 1: online-capable */
};

#define MADT_LAPIC_FLAG_ENABLED         (1u << 0)
#define MADT_LAPIC_FLAG_ONLINE_CAPABLE  (1u << 1)

#define ACPI_MAX_CPUS 16

/* Parse RSDP from the Multiboot2 info struct, establish the root SDT, parse
 * MCFG if present, and walk MADT for usable CPUs. Returns 0 when MADT was
 * parsed, -1 when ACPI or MADT is unavailable. Other valid tables remain
 * available even when MADT is absent. */
int      acpi_init(uint64_t mb2_addr);

/* Return the `index`th checksum-valid SDT with this four-byte signature.
 * The pointer remains valid for the lifetime of the kernel. */
const struct acpi_sdt_header *acpi_find_table(const char signature[4],
                                              uint32_t index);

/* Default LAPIC MMIO physical base reported by MADT. */
uint64_t acpi_lapic_phys(void);

/* Number of usable LOCAL_APIC entries (enabled + online-capable). */
int      acpi_cpu_count(void);

/* APIC ID for cpu `idx` (0..count-1). */
uint8_t  acpi_cpu_apic_id(int idx);

#endif
