/* kernel/drivers/video/nvidia.c - NVIDIA display discovery.
 *
 * GRUB/VBIOS already placed the display in a working mode and supplied a
 * linear framebuffer. Probe BAR0 just far enough to identify the GPU while
 * keeping that firmware scanout alive for generation-specific bring-up.
 */
#include <drivers/driver.h>
#include <drivers/video/nvidia/nvidia.h>
#include <display/framebuffer.h>
#include <memory/vmm.h>
#include <drivers/video/nvidia/nvidia_internal.h>
#include <pci/pci.h>
#include <utilities/log.h>
#include <stdint.h>

#define PCI_CLASS_DISPLAY 0x03
#define NVIDIA_MAX_DEVICES 4
#define PAGE_SIZE 4096ULL

/* NVIDIA's published nv_ref.h defines these as stable, chip-agnostic
 * identification registers in the BAR0 register aperture. */
#define NV_PMC_BOOT_0  0x00000000u
#define NV_PMC_BOOT_42 0x00000A00u

static struct nvidia_device nvidia_devices[NVIDIA_MAX_DEVICES];
static uint32_t _nvidia_device_count;

int nvidia_device_count(void) {
    return (int)_nvidia_device_count;
}

int nvidia_display_active(void) {
    for (uint32_t i = 0; i < _nvidia_device_count; i++) {
        if (nvidia_devices[i].state == NVIDIA_DISPLAY_READY)
            return 1;
    }
    return 0;
}

static int nvidia_match(const struct device *device) {
    if (device->bus != DEVICE_BUS_PCI)
        return 0;

    const struct pci_device *pci = &device->bus_info.pci;
    // log_write_hex("nvidia_match: vendor=", pci->vendor, KERNEL, LOG_INFO);
    // log_write_hex("nvidia_match: class_code=", pci->class_code, KERNEL, LOG_INFO);
    return pci->vendor == NVIDIA_PCI_VENDOR_ID
        && pci->class_code == PCI_CLASS_DISPLAY;
}

static int bar_contains(uint64_t address, const struct pci_bar *bar) {
    if (!bar->valid || bar->is_io || bar->size == 0)
        return 0;

    /* Subtraction avoids overflow in the usual address < base + size test. */
    return address >= bar->base && address - bar->base < bar->size;
}

static uint32_t nvidia_reg_read32(const struct nvidia_device *device,
                                  uint32_t offset) {
    return *(volatile uint32_t *)(device->regs + offset);
}

static uint32_t nvidia_boot0_architecture(uint32_t boot0) {
    uint32_t low = (boot0 >> 24) & 0x1Fu;
    uint32_t high = (boot0 >> 8) & 0x1u;
    return low | (high << 5);
}

static uint32_t nvidia_boot42_chip_id(uint32_t boot42) {
    /* NV_PMC_BOOT_42_CHIP_ID is bits 28:20 , nine bits (architecture in
     * 28:24, implementation in 23:20). A wider mask would leak reserved
     * bit 29 into the id. */
    return (boot42 >> 20) & 0x1FFu;
}

static uint32_t nvidia_boot42_implementation(uint32_t boot42) {
    return (boot42 >> 20) & 0xFu;
}

static const char *nvidia_architecture_name(uint32_t architecture) {
    switch (architecture) {
    case 0x11: return "Maxwell";
    case 0x12: return "Maxwell 2";
    case 0x13: return "Pascal";
    case 0x14:
    case 0x15: return "Volta";
    case 0x16: return "Turing";
    case 0x17: return "Ampere";
    case 0x18: return "Hopper";
    case 0x19: return "Ada";
    case 0x1A:
    case 0x1B: return "Blackwell";
    case 0x1C: return "Rubin";
    default: return "unknown";
    }
}

static int nvidia_map_control_bar(struct nvidia_device *state,
                                  uint32_t device_index) {
    const struct pci_bar *bar = &state->pci.bar[0];
    if (!bar->valid || bar->is_io || bar->base == 0
        || bar->size < NV_PMC_BOOT_42 + 4) {
        log_write("nvidia: BAR0 is not a usable register aperture",
                  KERNEL, LOG_ERROR);
        return -1;
    }
    if (bar->base & (PAGE_SIZE - 1)) {
        log_write("nvidia: BAR0 is not page aligned", KERNEL, LOG_ERROR);
        return -1;
    }

    uint16_t original_command =
        pci_read16(state->pci.addr, PCI_CFG_COMMAND);
    if (pci_enable_memory(&state->pci) != 0) {
        log_write("nvidia: could not enable PCI memory decoding",
                  KERNEL, LOG_ERROR);
        return -1;
    }

    uint64_t virt = NVIDIA_DEVICE_SLOT_BASE
                  + (uint64_t)device_index * NVIDIA_DEVICE_SLOT_SIZE;
    uint64_t flags = VMM_PRESENT | VMM_PCD | VMM_PWT | VMM_NX;
    if (vmm_map(virt, bar->base, flags) != 0) {
        pci_write16(state->pci.addr, PCI_CFG_COMMAND, original_command);
        log_write("nvidia: could not map BAR0 registers", KERNEL, LOG_ERROR);
        return -1;
    }

    state->regs = (volatile uint8_t *)(uintptr_t)virt;
    state->regs_phys = bar->base;
    state->regs_size = bar->size;
    state->boot0 = nvidia_reg_read32(state, NV_PMC_BOOT_0);
    state->boot42 = nvidia_reg_read32(state, NV_PMC_BOOT_42);

    if (state->boot0 == 0xFFFFFFFFu) {
        vmm_unmap(virt);
        pci_write16(state->pci.addr, PCI_CFG_COMMAND, original_command);
        state->regs = 0;
        log_write("nvidia: BAR0 register read returned all ones",
                  KERNEL, LOG_ERROR);
        return -1;
    }

    state->state = NVIDIA_MMIO_READY;
    log_write_hex("nvidia: PMC_BOOT_0 =", state->boot0,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: PMC_BOOT_42 =", state->boot42,
                  KERNEL, LOG_INFO);
    state->architecture = nvidia_boot0_architecture(state->boot0);
    state->implementation = nvidia_boot42_implementation(state->boot42);
    state->chip_id = nvidia_boot42_chip_id(state->boot42);
    log_write_hex("nvidia: architecture id =", state->architecture,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: chip id =", state->chip_id,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: implementation id =", state->implementation,
                  KERNEL, LOG_INFO);
    log_write_string("nvidia: architecture",
                     nvidia_architecture_name(state->architecture),
                     KERNEL, LOG_INFO);
    return 0;
}


static int nvidia_probe(struct device *device) {
    log_write("nvidia: starting probe", KERNEL, LOG_INFO);
    if (_nvidia_device_count >= NVIDIA_MAX_DEVICES) {
        log_write("nvidia: device table full", KERNEL, LOG_ERROR);
        return -1;
    }

    const struct pci_device *pci = &device->bus_info.pci;
    int has_mmio = 0;

    log_write_hex("nvidia: vendor:device =",
                  ((uint32_t)pci->vendor << 16) | pci->device,
                  KERNEL, LOG_INFO);
    log_write_hex("nvidia: subsystem =",
                  ((uint32_t)pci->subsys_vendor << 16) | pci->subsys_id,
                  KERNEL, LOG_INFO);

    for (int i = 0; i < 6; i++) {
        const struct pci_bar *bar = &pci->bar[i];
        if (!bar->valid || bar->is_io)
            continue;

        has_mmio = 1;
        log_write_hex("nvidia: MMIO BAR index =", (uint64_t)i,
                      KERNEL, LOG_INFO);
        log_write_hex("nvidia: MMIO BAR base =", bar->base,
                      KERNEL, LOG_INFO);
        log_write_hex("nvidia: MMIO BAR size =", bar->size,
                      KERNEL, LOG_INFO);
    }

    if (!has_mmio) {
        log_write("nvidia: display controller has no MMIO BAR",
                  KERNEL, LOG_ERROR);
        return -1;
    }

    struct nvidia_device *state = &nvidia_devices[_nvidia_device_count];
    state->pci = *pci;
    state->boot_framebuffer_bar = -1;
    state->state = NVIDIA_DETECTED;
    state->regs = 0;
    state->regs_phys = 0;
    state->regs_size = 0;
    state->boot0 = 0;
    state->boot42 = 0;
    state->architecture = 0;
    state->implementation = 0;
    state->chip_id = 0;
    state->firmware_family = NVIDIA_FW_FAMILY_NONE;
    state->gsp_firmware = 0;
    state->gsp_firmware_size = 0;
    state->gsp_image = 0;
    state->gsp_image_size = 0;
    state->gsp_signature = 0;
    state->gsp_signature_size = 0;
    state->ucode_firmware = 0;
    state->ucode_firmware_size = 0;
    state->gsp_radix3 = 0;
    state->ucode_radix3 = 0;
    state->firmware_version[0] = 0;
    state->fwsec_desc_offset = 0;
    state->fwsec_desc_version = 0;
    state->fwsec_desc_size = 0;
    state->fwsec_app_id = 0;
    state->fwsec_present = 0;

    uint64_t fb_phys = framebuffer_phys();
    for (int i = 0; i < 6; i++) {
        if (bar_contains(fb_phys, &pci->bar[i])) {
            state->boot_framebuffer_bar = i;
            log_write_hex("nvidia: boot framebuffer is in BAR", (uint64_t)i,
                          KERNEL, LOG_INFO);
            break;
        }
    }

    if (state->boot_framebuffer_bar < 0) {
        log_write("nvidia: boot framebuffer is outside decoded BARs",
                  KERNEL, LOG_WARN);
    }

    if (nvidia_map_control_bar(state, _nvidia_device_count) != 0) {
        state->state = NVIDIA_FAILED;
        return -1;
    }

    if (nvidia_vbios_load(state, _nvidia_device_count) != 0) {
        log_write("nvidia: VBIOS unavailable, native init deferred",
                  KERNEL, LOG_WARN);
    } else if (nvidia_fwsec_locate(state) != 0) {
        /* Not fatal here: the boot framebuffer stays up either way, and
         * WPR2 setup is the only thing that needs FWSEC. */
        log_write("nvidia: FWSEC ucode not located, WPR2 setup deferred",
                  KERNEL, LOG_WARN);
    }

    if (state->architecture >= 0x16 && state->architecture <= 0x1C) {
        if (state->state == NVIDIA_VBIOS_READY) {
            log_write("nvidia: ready for Turing+ firmware selection",
                      KERNEL, LOG_INFO);
        } else {
            log_write("nvidia: Turing+ device blocked on VBIOS access",
                      KERNEL, LOG_WARN);
        }
    } else {
        log_write("nvidia: generation is outside the Turing+ path",
                  KERNEL, LOG_WARN);
    }

    _nvidia_device_count++;
    device->driver_data = state;
    log_write("nvidia: using firmware boot framebuffer",
              KERNEL, LOG_INFO);
    return 0;
}


static const struct driver nvidia_driver = {
    .name = "nvidia-video",
    .bus = DEVICE_BUS_PCI,
    .match = nvidia_match,
    .probe = nvidia_probe,
};

int nvidia_driver_register(void) {
    _nvidia_device_count = 0;
    return driver_register(&nvidia_driver);
}

void nvidia_driver_late_init(void) {
    for (uint32_t i = 0; i < _nvidia_device_count; i++) {
        struct nvidia_device *device = &nvidia_devices[i];
        if (device->state != NVIDIA_VBIOS_READY)
            continue;

        log_write_hex("nvidia: loading firmware for device", i,
                      KERNEL, LOG_INFO);
        if (nvidia_firmware_load(device) != 0) {
            log_write("nvidia: firmware unavailable; retaining boot framebuffer",
                      KERNEL, LOG_WARN);
            continue;
        }

        if (nvidia_gsp_prepare(device) != 0) {
            log_write("nvidia: could not stage GSP image; retaining boot framebuffer",
                      KERNEL, LOG_WARN);
        }
    }
}
