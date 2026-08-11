/* NVIDIA FWSEC ucode location.
 *
 * FWSEC is the VBIOS-resident ucode that carves WPR2 out of VRAM before
 * GSP-RM can be booted. Finding it is a two-hop walk: the BIT table's
 * FALCON_DATA token points at a falcon ucode table, and one entry in that
 * table carries the FWSEC application id plus the offset of its descriptor.
 *
 * This stage stops at the descriptor's location. Decoding the descriptor
 * body (IMEM/DMEM spans, signature versions, interface offsets) is
 * deliberately not attempted yet — see the note above nvidia_fwsec_locate.
 */
#include "nvidia_internal.h"
#include "utilities/log.h"
#include <stdint.h>

/* Only version 1 of the falcon ucode table is known to carry FWSEC. */
#define FALCON_UCODE_TABLE_VERSION 1
#define FALCON_UCODE_TABLE_HDR_MIN 6
#define FALCON_UCODE_ENTRY_MIN     6

/* Application ids for the two FWSEC signing variants. Production parts
 * ship PROD; DBG appears on engineering samples. */
#define FALCON_APPID_FWSEC_DBG  0x45u
#define FALCON_APPID_FWSEC_PROD 0x85u

static uint32_t read_le32(const uint8_t *data, uint32_t offset) {
    return (uint32_t)data[offset]
         | ((uint32_t)data[offset + 1] << 8)
         | ((uint32_t)data[offset + 2] << 16)
         | ((uint32_t)data[offset + 3] << 24);
}

/* Locate the FWSEC entry and record where its descriptor starts.
 *
 * Unverified against real silicon: the falcon ucode table header/entry
 * layout and the FWSEC application id values below are taken from the
 * published NVIDIA/nouveau definitions but have not been checked against a
 * VBIOS dump from this machine. The descriptor body is therefore left
 * undecoded — everything needed to confirm the layout is logged instead.
 */
int nvidia_fwsec_locate(struct nvidia_device *device) {
    if (!device)
        return -1;

    device->fwsec_present = 0;
    device->fwsec_desc_offset = 0;
    device->fwsec_desc_version = 0;
    device->fwsec_desc_size = 0;
    device->fwsec_app_id = 0;

    if (!device->vbios || device->vbios_size == 0)
        return -1;

    struct nvidia_bit_token token;
    if (nvidia_bit_find_token(device, BIT_TOKEN_FALCON_DATA, &token) != 0) {
        log_write("nvidia: VBIOS has no BIT falcon data token",
                  KERNEL, LOG_WARN);
        return -1;
    }
    if (token.data_size < 4) {
        log_write("nvidia: BIT falcon data token is too short",
                  KERNEL, LOG_ERROR);
        return -1;
    }

    const uint8_t *image = device->vbios;
    uint32_t size = device->vbios_size;

    uint32_t table = read_le32(image, token.data_offset);
    if (table == 0 || table > size
        || size - table < FALCON_UCODE_TABLE_HDR_MIN) {
        log_write("nvidia: falcon ucode table pointer is out of range",
                  KERNEL, LOG_ERROR);
        return -1;
    }

    uint8_t version     = image[table];
    uint8_t header_size = image[table + 1];
    uint8_t entry_size  = image[table + 2];
    uint8_t entry_count = image[table + 3];
    uint8_t desc_version = image[table + 4];
    uint8_t desc_size    = image[table + 5];

    if (version != FALCON_UCODE_TABLE_VERSION
        || header_size < FALCON_UCODE_TABLE_HDR_MIN
        || entry_size < FALCON_UCODE_ENTRY_MIN) {
        log_write_hex("nvidia: unsupported falcon ucode table version =",
                      version, KERNEL, LOG_WARN);
        return -1;
    }

    uint64_t entries_end = (uint64_t)table + header_size
                         + (uint64_t)entry_size * entry_count;
    if (entries_end > size) {
        log_write("nvidia: falcon ucode table runs past the VBIOS",
                  KERNEL, LOG_ERROR);
        return -1;
    }

    log_write_hex("nvidia: falcon ucode table =", table, KERNEL, LOG_INFO);
    log_write_hex("nvidia: falcon ucode entries =", entry_count,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: falcon ucode desc version =", desc_version,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: falcon ucode desc size =", desc_size,
                  KERNEL, LOG_INFO);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t entry = table + header_size + i * entry_size;
        uint8_t app_id = image[entry];
        uint32_t desc_offset = read_le32(image, entry + 2);

        /* Log every entry: on unfamiliar silicon the id we want may not be
         * one of the two below, and the table is short enough that dumping
         * it costs nothing. */
        log_write_hex("nvidia: falcon ucode app id =", app_id,
                      KERNEL, LOG_INFO);
        log_write_hex("nvidia: falcon ucode desc offset =", desc_offset,
                      KERNEL, LOG_INFO);

        if (app_id != FALCON_APPID_FWSEC_DBG
            && app_id != FALCON_APPID_FWSEC_PROD)
            continue;

        /* Descriptor offsets are read as VBIOS-blob relative, matching the
         * convention the BIT token offsets use. Confirm against a dump
         * before decoding the descriptor body. */
        if (desc_offset > size || size - desc_offset < desc_size) {
            log_write("nvidia: FWSEC descriptor is out of range",
                      KERNEL, LOG_ERROR);
            return -1;
        }

        device->fwsec_desc_offset = desc_offset;
        device->fwsec_desc_version = desc_version;
        device->fwsec_desc_size = desc_size;
        device->fwsec_app_id = app_id;
        device->fwsec_present = 1;

        log_write_string("nvidia: FWSEC variant",
                         app_id == FALCON_APPID_FWSEC_PROD ? "prod" : "debug",
                         KERNEL, LOG_INFO);
        log_write_hex("nvidia: FWSEC descriptor at =", desc_offset,
                      KERNEL, LOG_INFO);
        return 0;
    }

    log_write("nvidia: no FWSEC entry in the falcon ucode table",
              KERNEL, LOG_WARN);
    return -1;
}
