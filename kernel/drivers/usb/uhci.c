#include "memory/pmm.h"
#include "memory/vmm.h"
#include "devices/io.h"
#include "utilities/log.h"
#include <stdint.h>
#include "devices/usb.h"
#include "pci/pci.h"

// UHCI specific hardware function
int uhci_send_control(struct hcd *hcd, uint8_t dev_addr, uint8_t endpoint, void *setup, void *buf, uint16_t len) {
    // ... UHCI magic: build Transfer Descriptors, add to Frame List, wait ...
    return 0;
}

#define UHCI_VIRT_FRAMELIST 0xFFFFFE0040000000ULL

int uhci_init(struct pci_device *dev) {
    // [Assuming the previous code: getting io_base, global reset, etc.]
    uint32_t io_base = (uint32_t)dev->bar[4].base;

    // 1. Allocate 1 physical frame (4 KB = 1024 x 4-byte pointers)
    // MUST be below 4GB so the 32-bit UHCI controller can address it!
    uint64_t frame_phys = pmm_alloc_frame_below(0x100000000ULL);
    if (!frame_phys) {
        log_write("UHCI: Failed to allocate physical frame.", KERNEL, LOG_ERROR);
        return -1;
    }

    // 2. Map it into the kernel's virtual space so we can write to it.
    // VMM_PCD and VMM_PWT disable caching, which is REQUIRED for DMA memory.
    if (vmm_map(UHCI_VIRT_FRAMELIST, frame_phys,
                VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT) != 0) {
        log_write("UHCI: Failed to map frame list.", KERNEL, LOG_ERROR);
        pmm_free_frame(frame_phys);
        return -1;
    }

    // 3. Zero out the frame list.
    // In UHCI, a 0 in the frame list means "do nothing this millisecond".
    // Note: UHCI uses 32-bit physical addresses in the frame list, even on 64-bit OSes!
    uint32_t *frame_list = (uint32_t *)UHCI_VIRT_FRAMELIST;
    for (int i = 0; i < 1024; i++) {
        frame_list[i] = 0;
    }

    // 4. Tell the UHCI controller where the Frame List is
    // The FRBASEADD register expects the PHYSICAL address.
    outl(io_base + 0x08, (uint32_t)frame_phys);

    // 5. Set the Frame Number to 0
    outw(io_base + 0x0C, 0x0000);

    // 6. Start the controller!
    // Bit 0 of USBCMD is the Run/Stop bit. Setting it to 1 starts the hardware.
    outw(io_base + 0x00, 0x01);

    log_write("UHCI: Controller started and Frame List configured.", KERNEL, LOG_INFO);

    // 7. Check if a device is plugged in!
    // UHCI_PORTSC1 is at offset 0x10
    uint16_t port_status = inw(io_base + 0x10);

    if (port_status & 0x01) { // Bit 0 is Current Connect Status
        log_write("UHCI: Device detected on Port 1!", KERNEL, LOG_INFO);

        // Now we need to reset the port to enable the device...
    } else {
        log_write("UHCI: Port 1 is empty.", KERNEL, LOG_INFO);
    }

    return 0;
}
