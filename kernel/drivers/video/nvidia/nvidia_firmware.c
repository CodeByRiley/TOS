/* NVIDIA GSP firmware selection and container validation.
 *
 * PCI probe runs before FAT is mounted, so this stage is invoked later from
 * kernel_main.  It follows NVIDIA's published architecture/implementation
 * selection and validates the required sections in the GSP ELF container.
 */
#include <firmware/firmware.h>
#include <drivers/video/nvidia/nvidia_internal.h>
#include <utilities/log.h>
#include <utilities/string.h>
#include <stddef.h>
#include <stdint.h>

struct PACKED elf64_header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct PACKED elf64_section {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t address;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t alignment;
    uint64_t entry_size;
};

struct firmware_choice {
    enum nvidia_firmware_family family;
    const char *family_name;
    const char *gsp_file;
    const char *ucode_file;
};

static int span_is_valid(uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}

static int section_name_equals(const char *name, uint64_t available,
                               const char *expected) {
    uint64_t i = 0;
    while (expected[i]) {
        if (i >= available || name[i] != expected[i])
            return 0;
        i++;
    }
    return i < available && name[i] == '\0';
}

#define SHT_NOBITS 8

static const struct elf64_section *find_section(const uint8_t *image,
                                                 uint64_t image_size,
                                                 const char *wanted) {
    const struct elf64_header *header =
        (const struct elf64_header *)image;
    const struct elf64_section *sections =
        (const struct elf64_section *)(image + header->shoff);
    const struct elf64_section *names = &sections[header->shstrndx];
    const char *name_table = (const char *)(image + names->offset);

    for (uint16_t i = 0; i < header->shnum; i++) {
        const struct elf64_section *section = &sections[i];
        /* Skip sections we are not looking for rather than rejecting the
         * whole container: a SHT_NOBITS section legitimately has a size
         * with no file bytes behind it, and an unrelated malformed
         * section is not our problem. Only the section we will actually
         * consume must have a valid file-backed span. */
        if (section->name >= names->size)
            continue;
        if (!section_name_equals(name_table + section->name,
                                 names->size - section->name, wanted))
            continue;

        if (section->type == SHT_NOBITS
            || !span_is_valid(section->offset, section->size, image_size))
            return 0;
        return section;
    }
    return 0;
}

static int build_signature_section_name(char *out, uint64_t out_size,
                                        const char *family_name) {
    static const char prefix[] = ".fwsignature_";
    uint64_t prefix_length = sizeof(prefix) - 1;
    uint64_t family_length = strlen(family_name);
    if (prefix_length + family_length + 1 > out_size)
        return -1;

    memcpy(out, prefix, prefix_length);
    memcpy(out + prefix_length, family_name, family_length + 1);
    return 0;
}

static int validate_gsp_container(struct nvidia_device *device,
                                  const char *family_name) {
    const uint8_t *image = (const uint8_t *)device->gsp_firmware;
    uint64_t size = device->gsp_firmware_size;
    if (!image || size < sizeof(struct elf64_header))
        return -1;

    const struct elf64_header *header =
        (const struct elf64_header *)image;
    if (header->ident[0] != 0x7F || header->ident[1] != 'E'
        || header->ident[2] != 'L' || header->ident[3] != 'F'
        || header->ident[4] != 2 || header->ident[5] != 1
        || header->ident[6] != 1
        || header->ehsize != sizeof(*header)
        || header->shentsize != sizeof(struct elf64_section)
        || header->shnum == 0 || header->shstrndx >= header->shnum)
        return -1;

    uint64_t section_table_size =
        (uint64_t)header->shentsize * header->shnum;
    if (!span_is_valid(header->shoff, section_table_size, size))
        return -1;

    const struct elf64_section *sections =
        (const struct elf64_section *)(image + header->shoff);
    const struct elf64_section *names = &sections[header->shstrndx];
    if (!span_is_valid(names->offset, names->size, size))
        return -1;

    const struct elf64_section *version =
        find_section(image, size, ".fwversion");
    const struct elf64_section *firmware_image =
        find_section(image, size, ".fwimage");
    char signature_name[32];
    if (build_signature_section_name(signature_name, sizeof(signature_name),
                                     family_name) != 0)
        return -1;
    const struct elf64_section *signature =
        find_section(image, size, signature_name);

    if (!version || !firmware_image || !signature || version->size == 0
        || version->size > sizeof(device->firmware_version)
        || firmware_image->size == 0 || signature->size == 0)
        return -1;

    const char *version_text = (const char *)(image + version->offset);
    if (version_text[version->size - 1] != '\0')
        return -1;

    memcpy(device->firmware_version, version_text, (size_t)version->size);
    device->firmware_version[sizeof(device->firmware_version) - 1] = '\0';
    device->gsp_image = image + firmware_image->offset;
    device->gsp_image_size = firmware_image->size;
    device->gsp_signature = image + signature->offset;
    device->gsp_signature_size = signature->size;
    return 0;
}

static struct firmware_choice select_firmware(uint32_t architecture,
                                               uint32_t implementation) {
    struct firmware_choice choice = {0};

    switch (architecture) {
    case 0x16:
        choice.family = implementation <= 0x06
                      ? NVIDIA_FW_FAMILY_TU10X
                      : NVIDIA_FW_FAMILY_TU11X;
        choice.family_name = implementation <= 0x06 ? "tu10x" : "tu11x";
        break;
    case 0x17:
        choice.family = implementation == 0
                      ? NVIDIA_FW_FAMILY_GA100
                      : NVIDIA_FW_FAMILY_GA10X;
        choice.family_name = implementation == 0 ? "ga100" : "ga10x";
        break;
    case 0x18:
        choice.family = NVIDIA_FW_FAMILY_GH100;
        choice.family_name = "gh100";
        break;
    case 0x19:
        choice.family = NVIDIA_FW_FAMILY_AD10X;
        choice.family_name = "ad10x";
        break;
    case 0x1A:
        choice.family = implementation == 0x0B
                      ? NVIDIA_FW_FAMILY_GB10Y
                      : NVIDIA_FW_FAMILY_GB10X;
        choice.family_name = implementation == 0x0B ? "gb10y" : "gb10x";
        break;
    case 0x1B:
        choice.family = implementation >= 0x0B
                      ? NVIDIA_FW_FAMILY_GB20Y
                      : NVIDIA_FW_FAMILY_GB20X;
        choice.family_name = implementation >= 0x0B ? "gb20y" : "gb20x";
        break;
    case 0x1C:
        choice.family = NVIDIA_FW_FAMILY_GR10X;
        choice.family_name = "gr10x";
        break;
    default:
        return choice;
    }

    /* NVIDIA publishes two external GSP/ucode package sets. */
    if (choice.family == NVIDIA_FW_FAMILY_TU10X
        || choice.family == NVIDIA_FW_FAMILY_TU11X
        || choice.family == NVIDIA_FW_FAMILY_GA100) {
        choice.gsp_file = "firmware/gsp_tu10x.bin";
        choice.ucode_file = "firmware/ucodes_tu10x.bin";
    } else {
        choice.gsp_file = "firmware/gsp_ga10x.bin";
        choice.ucode_file = "firmware/ucodes_ga10x.bin";
    }
    return choice;
}

int nvidia_firmware_load(struct nvidia_device *device) {
    if (!device || device->state != NVIDIA_VBIOS_READY)
        return -1;

    struct firmware_choice choice =
        select_firmware(device->architecture, device->implementation);
    if (choice.family == NVIDIA_FW_FAMILY_NONE) {
        log_write("nvidia: no GSP firmware family for this architecture",
                  KERNEL, LOG_WARN);
        return -1;
    }

    device->firmware_family = choice.family;
    log_write_string("nvidia: firmware chip family", choice.family_name,
                     KERNEL, LOG_INFO);
    log_write_string("nvidia: GSP firmware file", choice.gsp_file,
                     KERNEL, LOG_INFO);

    if (firmware_load(choice.gsp_file, &device->gsp_firmware,
                      &device->gsp_firmware_size) != 0) {
        log_write_string("nvidia: GSP firmware file not found",
                         choice.gsp_file, KERNEL, LOG_WARN);
        return -1;
    }

    if (validate_gsp_container(device, choice.family_name) != 0) {
        log_write("nvidia: invalid GSP ELF container", KERNEL, LOG_ERROR);
        firmware_release(device->gsp_firmware);
        device->gsp_firmware = 0;
        device->gsp_firmware_size = 0;
        device->gsp_image = 0;
        device->gsp_image_size = 0;
        device->gsp_signature = 0;
        device->gsp_signature_size = 0;
        return -1;
    }

    log_write_hex("nvidia: GSP firmware bytes =", device->gsp_firmware_size,
                  KERNEL, LOG_INFO);
    log_write_string("nvidia: GSP firmware version", device->firmware_version,
                     KERNEL, LOG_INFO);
    log_write_hex("nvidia: GSP image bytes =", device->gsp_image_size,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: GSP signature bytes =", device->gsp_signature_size,
                  KERNEL, LOG_INFO);

    /* Current NVIDIA code permits this archive to be absent.  Retain that
     * behavior until the external bindata path is implemented. */
    if (firmware_load(choice.ucode_file, &device->ucode_firmware,
                      &device->ucode_firmware_size) == 0) {
        log_write_hex("nvidia: ucode archive bytes =",
                      device->ucode_firmware_size, KERNEL, LOG_INFO);
    } else {
        log_write_string("nvidia: optional ucode archive not found",
                         choice.ucode_file, KERNEL, LOG_WARN);
    }

    device->state = NVIDIA_FIRMWARE_READY;
    log_write("nvidia: GSP firmware loaded and validated", KERNEL, LOG_INFO);
    return 0;
}
