/* src/intf/boot/multiboot2.h — Multiboot2 info struct + tag layouts.
 *
 * The bootloader (GRUB) hands us a chain of tags describing memory map,
 * framebuffer, modules, ACPI RSDP, etc. This header declares just the
 * tags the kernel reads at boot.
 *
 * Implementation: src/impl/kernel/boot/multiboot2.c.
 */
#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

/* Tag type IDs we care about. */
#define MULTIBOOT_TAG_END           0
#define MULTIBOOT_TAG_MODULE        3
#define MULTIBOOT_TAG_MMAP          6
#define MULTIBOOT_TAG_FRAMEBUFFER   8
#define MULTIBOOT_TAG_ACPI_OLD     14   /* RSDP v1 (ACPI 1.0) — 20-byte header */
#define MULTIBOOT_TAG_ACPI_NEW     15   /* XSDP v2+ — 36-byte header           */

/* Framebuffer color encodings reported by tag 8. */
#define FB_TYPE_INDEXED 0
#define FB_TYPE_RGB     1
#define FB_TYPE_EGA     2

/* Common header on every tag. `size` includes the header. */
struct MB2_TAG {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

/* Memory map entry (mmap tag payload). */
struct MB2_MMAP_ENTRY {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct MB2_TAG_MMAP {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct MB2_MMAP_ENTRY entries[];
} __attribute__((packed));

/* Module tag (GRUB modules loaded alongside the kernel). */
struct MB2_TAG_MODULE {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;     /* physical addr of module first byte         */
    uint32_t mod_end;       /* physical addr one past last byte           */
    char     cmdline[];     /* null-terminated string                     */
} __attribute__((packed));

/* Framebuffer description. */
struct MB2_TAG_FRAMEBUFFER {
    uint32_t type;
    uint32_t size;
    uint64_t addr;          /* physical */
    uint32_t pitch;         /* bytes per row */
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    uint8_t  red_pos;
    uint8_t  red_size;
    uint8_t  green_pos;
    uint8_t  green_size;
    uint8_t  blue_pos;
    uint8_t  blue_size;
} __attribute__((packed));

/* ACPI tag: bootloader copies the RSDP into payload[]. ACPI_OLD = 20-byte
 * v1 RSDP; ACPI_NEW = 36-byte v2 XSDP. Either may appear depending on
 * firmware — try ACPI_NEW first, fall back to ACPI_OLD. */
struct MB2_TAG_ACPI {
    uint32_t type;
    uint32_t size;
    uint8_t  rsdp[];
} __attribute__((packed));

/* Walk tags looking for one of the given `type`. Returns NULL if absent. */
struct MB2_TAG        *mb2_find_tag(uint64_t mb2_addr, uint32_t type);

/* Walk module tags looking for one whose cmdline matches `cmdline`. */
struct MB2_TAG_MODULE *mb2_find_module(uint64_t mb2_addr, const char *cmdline);

#endif
