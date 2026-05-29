#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* ACPI table structures we actually use. The full ACPI spec describes
 * dozens of tables; for SMP we only need RSDP -> RSDT/XSDT -> MADT, and
 * inside MADT only the LOCAL_APIC entries. */

#define ACPI_SIG_RSDP "RSD PTR "        /* 8 bytes, note the trailing space */
#define ACPI_SIG_RSDT "RSDT"
#define ACPI_SIG_XSDT "XSDT"
#define ACPI_SIG_APIC "APIC"            /* MADT signature */

struct __attribute__((packed)) acpi_rsdp_v1 {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;                  /* 0 = v1.0, 2 = v2.0+ */
    uint32_t rsdt_phys;
};

struct __attribute__((packed)) acpi_rsdp_v2 {
    struct acpi_rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
};

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

struct __attribute__((packed)) acpi_madt {
    struct acpi_sdt_header h;
    uint32_t lapic_phys;                /* default LAPIC MMIO base */
    uint32_t flags;                     /* bit 0: legacy PIC present */
    /* variable-length entries follow */
};

/* MADT entry types */
#define MADT_TYPE_LOCAL_APIC        0
#define MADT_TYPE_IO_APIC           1
#define MADT_TYPE_INT_OVERRIDE      2
#define MADT_TYPE_LOCAL_APIC_OVR    5
#define MADT_TYPE_LOCAL_X2APIC      9

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

int      acpi_init(uint64_t mb2_addr);   /* 0 = found, -1 = no ACPI / no MADT */
uint64_t acpi_lapic_phys(void);          /* default LAPIC MMIO physical base  */
int      acpi_cpu_count(void);           /* count of usable LOCAL_APIC entries */
uint8_t  acpi_cpu_apic_id(int idx);      /* APIC ID for cpu idx (0..count-1)  */

#endif
