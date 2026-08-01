/* Prepare system-memory inputs for a later NVIDIA GSP boot stage. */
#include "nvidia_internal.h"
#include "utilities/log.h"

int nvidia_gsp_prepare(struct nvidia_device *device) {
    if (!device || device->state != NVIDIA_FIRMWARE_READY
        || !device->gsp_image || device->gsp_image_size == 0
        || !device->gsp_signature || device->gsp_signature_size == 0)
        return -1;

    if (nvidia_radix3_build(&device->gsp_radix3, device->gsp_image,
                            device->gsp_image_size) != 0) {
        log_write("nvidia: failed to build GSP image radix", KERNEL,
                  LOG_ERROR);
        return -1;
    }

    if (device->ucode_firmware && device->ucode_firmware_size != 0
        && nvidia_radix3_build(&device->ucode_radix3,
                               device->ucode_firmware,
                               device->ucode_firmware_size) != 0) {
        log_write("nvidia: failed to build ucode archive radix", KERNEL,
                  LOG_ERROR);
        nvidia_radix3_destroy(device->gsp_radix3);
        device->gsp_radix3 = 0;
        return -1;
    }

    log_write_hex("nvidia: GSP radix root =",
                  nvidia_radix3_root_phys(device->gsp_radix3),
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: GSP radix table pages =",
                  nvidia_radix3_table_pages(device->gsp_radix3),
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: GSP radix data pages =",
                  nvidia_radix3_data_pages(device->gsp_radix3),
                  KERNEL, LOG_INFO);

    if (device->ucode_radix3) {
        log_write_hex("nvidia: ucode radix root =",
                      nvidia_radix3_root_phys(device->ucode_radix3),
                      KERNEL, LOG_INFO);
    }

    device->state = NVIDIA_GSP_IMAGE_READY;
    log_write("nvidia: GSP system-memory images staged", KERNEL, LOG_INFO);
    return 0;
}
