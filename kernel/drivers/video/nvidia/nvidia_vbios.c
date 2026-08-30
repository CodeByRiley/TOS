/* NVIDIA VBIOS acquisition and structural validation. */
#include <drivers/video/nvidia/nvidia_internal.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/vmm.h>
#include <utilities/log.h>
#include <stdint.h>

#define PAGE_SIZE 4096ULL

/* The shadow copy the system BIOS leaves at the legacy option-ROM window.
 * Used as a fallback when the card's own ROM BAR cannot be read. */
#define LEGACY_VBIOS_PHYS 0x000C0000ULL
#define LEGACY_VBIOS_SIZE 0x00020000ULL

/* ROM image signatures. Most of these are ASCII read back as a little-endian
 * word, which is why they look like arbitrary hex:
 *
 *   0xAA55     | "\x55\xAA" | Standard PCI option ROM header
 *   0x4E56     | "VN"       | NVIDIA-specific ROM header variant
 *   0xBB77     | ,          | NVIDIA extended ROM header variant
 *   0x52494350 | "PCIR"     | PCI Data Structure
 *   0x5344504E | "NPDS"     | NVIDIA Data Structure
 *   0x53494752 | "RGIS"     | NVIDIA signed-image Data Structure
 *   0x4544504E | "NPDE"     | NVIDIA extended Data Structure
 *   0x00544942 | "BIT\0"    | BIOS Information Table
 */
#define PCI_ROM_SIGNATURE       0xAA55u
#define PCI_ROM_SIGNATURE_NV    0x4E56u
#define PCI_ROM_SIGNATURE_NV2   0xBB77u
#define PCI_DATA_SIGNATURE      0x52494350u
#define PCI_DATA_SIGNATURE_NV   0x5344504Eu
#define PCI_DATA_SIGNATURE_NV2  0x53494752u
#define PCI_DATA_EXT_SIGNATURE  0x4544504Eu
#define PCI_DATA_EXT_REV_10     0x0100u
#define PCI_DATA_EXT_REV_11     0x0101u

/* Indicator byte in the PCI Data Structure: set on the final image of the
 * ROM, so image walking stops there. */
#define PCI_ROM_LAST_IMAGE      0x80u

/* ROM image lengths are counted in 512-byte blocks, not bytes. */
#define PCI_ROM_BLOCK_SIZE      512u

#define BIT_HEADER_ID        0xB8FFu
#define BIT_HEADER_SIGNATURE 0x00544942u
/* BIT_TOKEN_* ids live in nvidia_internal.h , later stages need them too. */

struct vbios_layout {
    u32 size;
    u32 image_count;
};

static u16 read_le16(const volatile u8 *data, u32 offset) {
    return (u16)data[offset]
         | ((u16)data[offset + 1] << 8);
}

static u32 read_le32(const volatile u8 *data, u32 offset) {
    return (u32)data[offset]
         | ((u32)data[offset + 1] << 8)
         | ((u32)data[offset + 2] << 16)
         | ((u32)data[offset + 3] << 24);
}

static int valid_rom_signature(u16 signature) {
    return signature == PCI_ROM_SIGNATURE
        || signature == PCI_ROM_SIGNATURE_NV
        || signature == PCI_ROM_SIGNATURE_NV2;
}

static int valid_data_signature(u32 signature) {
    return signature == PCI_DATA_SIGNATURE
        || signature == PCI_DATA_SIGNATURE_NV
        || signature == PCI_DATA_SIGNATURE_NV2;
}

static int nvidia_vbios_layout(const volatile u8 *rom,
                               u32 available,
                               struct vbios_layout *layout) {
    u32 image_offset = 0;
    u32 image_count = 0;

    while (image_count < 64) {
        if (image_offset > available || available - image_offset < 0x1A)
            return -1;
        if (!valid_rom_signature(read_le16(rom, image_offset)))
            return -1;

        u32 data_offset = read_le16(rom, image_offset + 0x18);
        if (data_offset > available - image_offset
            || available - image_offset - data_offset < 0x16)
            return -1;

        u32 data = image_offset + data_offset;
        if (!valid_data_signature(read_le32(rom, data)))
            return -1;
        if (read_le16(rom, data + 4) != NVIDIA_PCI_VENDOR_ID)
            return -1;

        u32 blocks = read_le16(rom, data + 0x10);
        u32 data_length = read_le16(rom, data + 0x0A);
        u8 last_image = rom[data + 0x15] & PCI_ROM_LAST_IMAGE;
        if (data_length < 0x16)
            return -1;

        /* NVIDIA's NPDE extension can describe a private sub-image shorter
         * than the enclosing standard PCI image. */
        if (data_length <= available - data) {
            u32 ext = (data + data_length + 15u) & ~15u;
            if (ext <= available && available - ext >= 12
                && read_le32(rom, ext) == PCI_DATA_EXT_SIGNATURE) {
                u16 revision = read_le16(rom, ext + 4);
                u16 ext_length = read_le16(rom, ext + 6);
                if (revision == PCI_DATA_EXT_REV_10
                    || revision == PCI_DATA_EXT_REV_11) {
                    u32 subimage_blocks = read_le16(rom, ext + 8);
                    if (subimage_blocks != 0)
                        blocks = subimage_blocks;
                    if (ext_length > 0x0A)
                        last_image = rom[ext + 0x0A] & PCI_ROM_LAST_IMAGE;
                }
            }
        }

        if (blocks == 0 || blocks > UINT32_MAX / PCI_ROM_BLOCK_SIZE)
            return -1;
        u32 image_size = blocks * PCI_ROM_BLOCK_SIZE;
        if (image_size > available - image_offset)
            return -1;

        image_offset += image_size;
        image_count++;
        if (last_image) {
            layout->size = image_offset;
            layout->image_count = image_count;
            return 0;
        }
    }

    return -1;
}

static void copy_from_volatile(u8 *destination,
                               const volatile u8 *source,
                               u32 size) {
    for (u32 i = 0; i < size; i++)
        destination[i] = source[i];
}

static int nvidia_find_bit(struct nvidia_device *device) {
    const u8 *image = device->vbios;
    u32 size = device->vbios_size;

    for (u32 offset = 0; offset + 14 <= size; offset++) {
        if (read_le16(image, offset) != BIT_HEADER_ID
            || read_le32(image, offset + 2) != BIT_HEADER_SIGNATURE)
            continue;

        u32 header_size = image[offset + 8];
        u32 token_size = image[offset + 9];
        u32 token_count = image[offset + 10];
        if (header_size < 14 || header_size > size - offset
            || token_size < 6)
            continue;

        u32 checksum = 0;
        for (u32 i = 0; i < header_size; i++)
            checksum += image[offset + i];
        if ((checksum & 0xFFu) != 0)
            continue;

        u64 tokens_end = (u64)offset + header_size
                            + (u64)token_size * token_count;
        if (tokens_end > size)
            continue;

        device->vbios_bit_offset = offset;
        device->vbios_bit_header_size = header_size;
        device->vbios_bit_token_size = token_size;
        device->vbios_bit_token_count = token_count;
        for (u32 i = 0; i < token_count; i++) {
            u32 token = offset + header_size + i * token_size;
            if (image[token] != BIT_TOKEN_BIOSDATA)
                continue;
            if (image[token + 1] != 1 && image[token + 1] != 2)
                continue;
            if (read_le16(image, token + 2) < 5)
                continue;

            u32 data = token_size >= 8
                          ? read_le32(image, token + 4)
                          : read_le16(image, token + 4);
            if (data > size || size - data < 5)
                continue;
            device->vbios_version = ((u64)read_le32(image, data) << 8)
                                  | image[data + 4];
            break;
        }
        return 0;
    }

    return -1;
}

/* Walk the BIT token array for `id`. nvidia_find_bit has already proved the
 * whole array lies inside the blob, so indexing tokens needs no further
 * bounds check; the token's own data span still does.
 *
 * Token data offsets are relative to the start of the VBIOS blob, not to the
 * BIT header. That holds because the BIT table lives in image 0 (the legacy
 * x86 ROM) and image 0 starts at blob offset 0. */
int nvidia_bit_find_token(const struct nvidia_device *device, u8 id,
                          struct nvidia_bit_token *out) {
    if (!device || !out || !device->vbios
        || device->vbios_bit_token_count == 0
        || device->vbios_bit_token_size < 6)
        return -1;

    const u8 *image = device->vbios;
    u32 size = device->vbios_size;
    u32 token_size = device->vbios_bit_token_size;
    u32 base = device->vbios_bit_offset + device->vbios_bit_header_size;

    for (u32 i = 0; i < device->vbios_bit_token_count; i++) {
        u32 token = base + i * token_size;
        if (image[token] != id)
            continue;

        u32 data_size = read_le16(image, token + 2);
        u32 data_offset = token_size >= 8
                             ? read_le32(image, token + 4)
                             : read_le16(image, token + 4);
        if (data_offset > size || size - data_offset < data_size)
            return -1;

        out->id = id;
        out->data_version = image[token + 1];
        out->data_size = (u16)data_size;
        out->data_offset = data_offset;
        return 0;
    }
    return -1;
}

static int copy_valid_vbios(struct nvidia_device *device,
                            const volatile u8 *source,
                            u32 available,
                            enum nvidia_vbios_source source_kind) {
    struct vbios_layout layout;
    if (nvidia_vbios_layout(source, available, &layout) != 0)
        return -1;

    u8 *copy = kmalloc(layout.size);
    if (!copy)
        return -1;
    copy_from_volatile(copy, source, layout.size);

    device->vbios = copy;
    device->vbios_size = layout.size;
    device->vbios_image_count = layout.image_count;
    device->vbios_source = source_kind;
    device->vbios_bit_offset = 0;
    device->vbios_bit_header_size = 0;
    device->vbios_bit_token_size = 0;
    device->vbios_bit_token_count = 0;
    device->vbios_version = 0;
    nvidia_find_bit(device);
    return 0;
}

static int load_from_pci_rom(struct nvidia_device *device,
                             u32 device_index) {
    const struct pci_rom *rom = &device->pci.rom;
    if (!rom->valid || rom->base == 0 || rom->size == 0)
        return -1;

    u64 size = rom->size;
    if (size > NVIDIA_MAX_VBIOS_SIZE)
        size = NVIDIA_MAX_VBIOS_SIZE;
    u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 virt = NVIDIA_DEVICE_SLOT_BASE
                  + (u64)device_index * NVIDIA_DEVICE_SLOT_SIZE
                  + NVIDIA_ROM_SLOT_OFFSET;

    u64 mapped = 0;
    for (; mapped < pages; mapped++) {
        if (vmm_map(virt + mapped * PAGE_SIZE,
                    rom->base + mapped * PAGE_SIZE,
                    VMM_PRESENT | VMM_PCD | VMM_PWT | VMM_NX) != 0)
            break;
    }
    if (mapped != pages) {
        while (mapped > 0)
            vmm_unmap(virt + --mapped * PAGE_SIZE);
        return -1;
    }

    u32 original =
        pci_read32(device->pci.addr, PCI_CFG_ROM_ADDRESS);
    pci_write32(device->pci.addr, PCI_CFG_ROM_ADDRESS,
                    (original & PCI_ROM_ADDR_MASK) | PCI_ROM_ENABLE);
    (void)pci_read32(device->pci.addr, PCI_CFG_ROM_ADDRESS);

    int result = copy_valid_vbios(device,
                                  (volatile u8 *)(uintptr_t)virt,
                                  (u32)size,
                                  NVIDIA_VBIOS_PCI_ROM);

    pci_write32(device->pci.addr, PCI_CFG_ROM_ADDRESS, original);
    for (u64 i = 0; i < pages; i++)
        vmm_unmap(virt + i * PAGE_SIZE);
    return result;
}

int nvidia_vbios_load(struct nvidia_device *device, u32 device_index) {
    device->vbios = 0;
    device->vbios_size = 0;
    device->vbios_image_count = 0;
    device->vbios_bit_offset = 0;
    device->vbios_bit_header_size = 0;
    device->vbios_bit_token_size = 0;
    device->vbios_bit_token_count = 0;
    device->vbios_version = 0;
    device->vbios_source = NVIDIA_VBIOS_NONE;

    if (load_from_pci_rom(device, device_index) != 0) {
        /* The legacy C0000 shadow belongs to the firmware-selected primary
         * adapter. Never assign that image to an unrelated secondary GPU. */
        if (device->boot_framebuffer_bar < 0)
            return -1;
        const volatile u8 *shadow = phys_to_virt(LEGACY_VBIOS_PHYS);
        if (copy_valid_vbios(device, shadow, LEGACY_VBIOS_SIZE,
                             NVIDIA_VBIOS_LEGACY_SHADOW) != 0)
            return -1;
    }

    device->state = NVIDIA_VBIOS_READY;
    log_write_string("nvidia: VBIOS source",
                     device->vbios_source == NVIDIA_VBIOS_PCI_ROM
                         ? "PCI expansion ROM" : "legacy shadow",
                     KERNEL, LOG_INFO);
    log_write_hex("nvidia: VBIOS size =", device->vbios_size,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: VBIOS images =", device->vbios_image_count,
                  KERNEL, LOG_INFO);
    if (device->vbios_bit_offset || device->vbios_version) {
        log_write_hex("nvidia: BIT offset =", device->vbios_bit_offset,
                      KERNEL, LOG_INFO);
        log_write_hex("nvidia: VBIOS version =", device->vbios_version,
                      KERNEL, LOG_INFO);
    } else {
        log_write("nvidia: VBIOS has no valid BIT table",
                  KERNEL, LOG_WARN);
    }
    return 0;
}
