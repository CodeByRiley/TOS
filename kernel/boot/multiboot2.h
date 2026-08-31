/* kernel/boot/multiboot2.h , Multiboot2 info struct + tag layouts.
 *
 * The bootloader (GRUB) hands us a chain of tags describing memory map,
 * framebuffer, modules, ACPI RSDP, etc. This header declares just the
 * tags the kernel reads at boot.
 *
 * Implementation: kernel/boot/multiboot2.c.
 */
#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H


/* PACKED and friends. */
#include <utilities/types.h>
#include <stdint.h>

/* Tag type IDs we care about. */
#define MULTIBOOT_TAG_END           0
#define MULTIBOOT_TAG_MODULE        3
#define MULTIBOOT_TAG_MMAP          6
#define MULTIBOOT_TAG_FRAMEBUFFER   8
#define MULTIBOOT_TAG_EFI32        11
#define MULTIBOOT_TAG_EFI64        12
#define MULTIBOOT_TAG_ACPI_OLD     14   /* RSDP v1 (ACPI 1.0) , 20-byte header */
#define MULTIBOOT_TAG_ACPI_NEW     15   /* XSDP v2+ , 36-byte header           */
#define MULTIBOOT_TAG_EFI_MMAP     17
#define MULTIBOOT_TAG_EFI_BS       18
#define MULTIBOOT_TAG_EFI32_IH     19
#define MULTIBOOT_TAG_EFI64_IH     20

/* Framebuffer color encodings reported by tag 8. */
#define FB_TYPE_INDEXED 0
#define FB_TYPE_RGB     1
#define FB_TYPE_EGA     2

/* Common header on every tag. `size` includes the header. */
struct MB2_TAG {
    u32 type;
    u32 size;
} PACKED;

/* Memory map entry (mmap tag payload). */
struct MB2_MMAP_ENTRY {
    u64 base;
    u64 len;
    u32 type;
    u32 reserved;
} PACKED;

struct MB2_TAG_MMAP {
    u32 type;
    u32 size;
    u32 entry_size;
    u32 entry_version;
    struct MB2_MMAP_ENTRY entries[];
} PACKED;

/* Module tag (GRUB modules loaded alongside the kernel). */
struct MB2_TAG_MODULE {
    u32 type;
    u32 size;
    u32 mod_start;     /* physical addr of module first byte         */
    u32 mod_end;       /* physical addr one past last byte           */
    char     cmdline[];     /* null-terminated string                     */
} PACKED;

/* Framebuffer description. */
struct MB2_TAG_FRAMEBUFFER {
    u32 type;
    u32 size;
    u64 addr;          /* physical */
    u32 pitch;         /* bytes per row */
    u32 width;
    u32 height;
    u8  bpp;
    u8  fb_type;
    u16 reserved;
    u8  red_pos;
    u8  red_size;
    u8  green_pos;
    u8  green_size;
    u8  blue_pos;
    u8  blue_size;
} PACKED;

/* ACPI tag: bootloader copies the RSDP into payload[]. ACPI_OLD = 20-byte
 * v1 RSDP; ACPI_NEW = 36-byte v2 XSDP. Either may appear depending on
 * firmware , try ACPI_NEW first, fall back to ACPI_OLD. */
struct MB2_TAG_ACPI {
    u32 type;
    u32 size;
    u8  rsdp[];
} PACKED;

/* EFI handoff tags. The pointers are firmware addresses supplied by GRUB;
 * merely recording them is safe after ExitBootServices, but callers must not
 * assume that boot-service function pointers remain callable. */
struct MB2_TAG_EFI32_PTR {
    u32 type;
    u32 size;
    u32 pointer;
} PACKED;

struct MB2_TAG_EFI64_PTR {
    u32 type;
    u32 size;
    u64 pointer;
} PACKED;

struct MB2_TAG_EFI_MMAP {
    u32 type;
    u32 size;
    u32 descriptor_size;
    u32 descriptor_version;
    u8  descriptors[];
} PACKED;

_Static_assert(sizeof(struct MB2_TAG_EFI32_PTR) == 12,
               "Multiboot EFI32 pointer tag layout");
_Static_assert(sizeof(struct MB2_TAG_EFI64_PTR) == 16,
               "Multiboot EFI64 pointer tag layout");
_Static_assert(sizeof(struct MB2_TAG_EFI_MMAP) == 16,
               "Multiboot EFI memory-map tag layout");

/* Walk tags looking for one of the given `type`. Returns NULL if absent. */
struct MB2_TAG        *mb2_find_tag(u64 mb2_addr, u32 type);

/* Walk module tags looking for one whose cmdline matches `cmdline`. */
struct MB2_TAG_MODULE *mb2_find_module(u64 mb2_addr, const char *cmdline);

#endif
