/* kernel/acpi/pci_mcfg.c - ACPI MCFG allocation parser. */
#include <acpi/pci_mcfg.h>
#include <utilities/log.h>
#include <stdint.h>

static struct pci_mcfg_range ranges[PCI_MCFG_MAX_RANGES];
static u32 range_count;

static int sig_is_mcfg(const char signature[4]) {
    return signature[0] == 'M' && signature[1] == 'C'
        && signature[2] == 'F' && signature[3] == 'G';
}

static int overlaps_existing(const struct pci_mcfg_range *candidate) {
    for (u32 i = 0; i < range_count; i++) {
        const struct pci_mcfg_range *range = &ranges[i];
        if (range->segment != candidate->segment)
            continue;
        if (candidate->start_bus <= range->end_bus
            && range->start_bus <= candidate->end_bus)
            return 1;
    }
    return 0;
}

int pci_mcfg_init(const struct acpi_sdt_header *table) {
    range_count = 0;
    if (!table)
        return -1;
    if (!sig_is_mcfg(table->signature)
        || table->length < sizeof(struct acpi_mcfg))
        return -1;

    u32 payload = table->length - sizeof(struct acpi_mcfg);
    if (payload % sizeof(struct acpi_mcfg_allocation) != 0)
        return -1;

    u32 entries = payload / sizeof(struct acpi_mcfg_allocation);
    const struct acpi_mcfg_allocation *allocations =
        (const struct acpi_mcfg_allocation*)
        ((const u8*)table + sizeof(struct acpi_mcfg));

    for (u32 i = 0; i < entries; i++) {
        const struct acpi_mcfg_allocation *entry = &allocations[i];
        struct pci_mcfg_range candidate = {
            .base_phys = entry->base_phys,
            .segment = entry->segment,
            .start_bus = entry->start_bus,
            .end_bus = entry->end_bus,
        };

        /* Each bus owns 1 MiB of ECAM address space. */
        if (!candidate.base_phys
            || (candidate.base_phys & ((1ULL << 20) - 1)) != 0
            || candidate.start_bus > candidate.end_bus) {
            log_write("ACPI: ignoring invalid MCFG allocation",
                      KERNEL, LOG_WARN);
            continue;
        }
        if (overlaps_existing(&candidate)) {
            log_write("ACPI: ignoring overlapping MCFG allocation",
                      KERNEL, LOG_WARN);
            continue;
        }
        if (range_count >= PCI_MCFG_MAX_RANGES) {
            log_write("ACPI: MCFG range table full", KERNEL, LOG_WARN);
            break;
        }

        ranges[range_count++] = candidate;
        log_write_hex("ACPI: MCFG ECAM base =", candidate.base_phys,
                      KERNEL, LOG_INFO);
        log_write_hex("ACPI: MCFG segment   =", candidate.segment,
                      KERNEL, LOG_INFO);
        log_write_hex("ACPI: MCFG start bus =", candidate.start_bus,
                      KERNEL, LOG_INFO);
        log_write_hex("ACPI: MCFG end bus   =", candidate.end_bus,
                      KERNEL, LOG_INFO);
    }

    return range_count ? 0 : -1;
}

u32 pci_mcfg_range_count(void) {
    return range_count;
}

int pci_mcfg_range_at(u32 index, struct pci_mcfg_range *out) {
    if (!out || index >= range_count)
        return 0;
    *out = ranges[index];
    return 1;
}

int pci_mcfg_find(u16 segment, u8 bus,
                  struct pci_mcfg_range *out) {
    for (u32 i = 0; i < range_count; i++) {
        const struct pci_mcfg_range *range = &ranges[i];
        if (range->segment == segment
            && bus >= range->start_bus
            && bus <= range->end_bus) {
            if (out)
                *out = *range;
            return 1;
        }
    }
    return 0;
}
