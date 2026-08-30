/* Private state shared by the NVIDIA probe and its bring-up stages. */
#ifndef NVIDIA_INTERNAL_H
#define NVIDIA_INTERNAL_H

#include <utilities/types.h>
#include <drivers/video/nvidia/nvidia.h>
#include <pci/pci.h>
#include <stdint.h>

#define NVIDIA_DEVICE_SLOT_BASE 0xFFFFE00400000000ULL
#define NVIDIA_DEVICE_SLOT_SIZE 0x200000ULL
#define NVIDIA_ROM_SLOT_OFFSET  0x100000ULL
#define NVIDIA_MAX_VBIOS_SIZE   0x100000ULL

enum nvidia_state {
    NVIDIA_DETECTED = 0,
    NVIDIA_MMIO_READY,
    NVIDIA_VBIOS_READY,
    NVIDIA_FIRMWARE_READY,
    NVIDIA_GSP_IMAGE_READY,
    NVIDIA_DISPLAY_READY,
    NVIDIA_FAILED,
};

enum nvidia_vbios_source {
    NVIDIA_VBIOS_NONE = 0,
    NVIDIA_VBIOS_PCI_ROM,
    NVIDIA_VBIOS_LEGACY_SHADOW,
};

/* Values follow NVIDIA's published nv_firmware_chip_family_t. */
enum nvidia_firmware_family {
    NVIDIA_FW_FAMILY_NONE  = 0,
    NVIDIA_FW_FAMILY_TU10X = 1,
    NVIDIA_FW_FAMILY_TU11X = 2,
    NVIDIA_FW_FAMILY_GA100 = 3,
    NVIDIA_FW_FAMILY_GA10X = 4,
    NVIDIA_FW_FAMILY_AD10X = 5,
    NVIDIA_FW_FAMILY_GH100 = 6,
    NVIDIA_FW_FAMILY_GB10X = 8,
    NVIDIA_FW_FAMILY_GB20X = 9,
    NVIDIA_FW_FAMILY_GR10X = 10,
    NVIDIA_FW_FAMILY_GB10Y = 11,
    NVIDIA_FW_FAMILY_GB20Y = 12,
};

/* BIT token ids. 'B' is the version/BIOSDATA token nvidia_find_bit already
 * consumes; 'p' carries the falcon ucode table pointer FWSEC lives in. */
#define BIT_TOKEN_BIOSDATA    0x42u
#define BIT_TOKEN_FALCON_DATA 0x70u

/* One decoded BIT token. `data_offset` is an offset into the VBIOS blob,
 * already bounds-checked against `data_size`. */
struct nvidia_bit_token {
    u8  id;
    u8  data_version;
    u16 data_size;
    u32 data_offset;
};

struct nvidia_radix3;

struct nvidia_device {
    struct pci_device pci;
    int boot_framebuffer_bar;
    enum nvidia_state state;
    volatile u8 *regs;
    u64 regs_phys;
    u64 regs_size;
    u32 boot0;
    u32 boot42;
    u32 architecture;
    u32 implementation;
    u32 chip_id;
    u8 *vbios;
    u32 vbios_size;
    u32 vbios_image_count;
    u32 vbios_bit_offset;
    /* BIT table geometry, retained so later stages can re-walk the token
     * array without rescanning for the header. */
    u32 vbios_bit_header_size;
    u32 vbios_bit_token_size;
    u32 vbios_bit_token_count;
    u64 vbios_version;
    /* FWSEC ucode descriptor location within the VBIOS. The descriptor
     * body is not decoded yet , see nvidia_fwsec.c. */
    u32 fwsec_desc_offset;
    u8  fwsec_desc_version;
    u8  fwsec_desc_size;
    u8  fwsec_app_id;
    u8  fwsec_present;
    enum nvidia_vbios_source vbios_source;
    enum nvidia_firmware_family firmware_family;
    const void *gsp_firmware;
    u64 gsp_firmware_size;
    const void *gsp_image;
    u64 gsp_image_size;
    const void *gsp_signature;
    u64 gsp_signature_size;
    const void *ucode_firmware;
    u64 ucode_firmware_size;
    struct nvidia_radix3 *gsp_radix3;
    struct nvidia_radix3 *ucode_radix3;
    char firmware_version[64];
};

int nvidia_vbios_load(struct nvidia_device *device, u32 device_index);

/* Find a BIT token by id. Returns 0 and fills *out on success. */
int nvidia_bit_find_token(const struct nvidia_device *device, u8 id,
                          struct nvidia_bit_token *out);

/* Locate the FWSEC entry in the VBIOS falcon ucode table. */
int nvidia_fwsec_locate(struct nvidia_device *device);

int nvidia_firmware_load(struct nvidia_device *device);
int nvidia_gsp_prepare(struct nvidia_device *device);

int nvidia_radix3_build(struct nvidia_radix3 **out, const void *data,
                        u64 size);
void nvidia_radix3_destroy(struct nvidia_radix3 *radix);
u64 nvidia_radix3_root_phys(const struct nvidia_radix3 *radix);
u64 nvidia_radix3_data_size(const struct nvidia_radix3 *radix);
u64 nvidia_radix3_table_pages(const struct nvidia_radix3 *radix);
u64 nvidia_radix3_data_pages(const struct nvidia_radix3 *radix);

#endif
