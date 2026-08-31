#ifndef PCI_TABLE_H
#define PCI_TABLE_H

#include <stdint.h>
#include <utilities/types.h>
#include "vendors/pci_ids.h"

struct pci_device_desc {
    u16 vendor;
    u16 device;
    const char *name;
};

static const struct pci_device_desc pci_known_devices[] = {
    { PCI_VENDOR_INTEL,   PCI_DEVICE_INTEL_E1000,   "Intel 82540EM (e1000)" },
    { PCI_VENDOR_INTEL,   PCI_DEVICE_INTEL_E1000E,  "Intel 82574L (e1000e)" },
    { PCI_VENDOR_INTEL,   PCI_DEVICE_INTEL_I217_LM, "Intel I217-LM" },
    { PCI_VENDOR_INTEL,   PCI_DEVICE_INTEL_I219_V,  "Intel I219-V" },

    { PCI_VENDOR_REALTEK, PCI_DEVICE_REALTEK_RTL8139, "Realtek RTL8139" },
    { PCI_VENDOR_REALTEK, PCI_DEVICE_REALTEK_RTL8168, "Realtek RTL8168" },
    { PCI_VENDOR_REALTEK, PCI_DEVICE_REALTEK_RTL8111, "Realtek RTL8111" },

    { PCI_VENDOR_VIRTIO,  PCI_DEVICE_VIRTIO_NET, "VirtIO Network" },
    { PCI_VENDOR_AMD,     PCI_DEVICE_AMD_PCNET,  "AMD PCnet" },

    { PCI_VENDOR_BROADCOM, PCI_DEVICE_BROADCOM_BCM5751,  "Broadcom BCM5751" },
    { PCI_VENDOR_BROADCOM, PCI_DEVICE_BROADCOM_BCM57765, "Broadcom BCM57765" },

    { PCI_VENDOR_ATHEROS, PCI_DEVICE_ATHEROS_AR8161, "Atheros AR8161" },
    { PCI_VENDOR_ATHEROS, PCI_DEVICE_ATHEROS_AR8162, "Atheros AR8162" },

    { PCI_VENDOR_MARVELL, PCI_DEVICE_MARVELL_YUKON, "Marvell Yukon" },


};

SINLINE const char *pci_device_name(u16 vendor, u16 device)
{
    for (unsigned i = 0; i < (sizeof(pci_known_devices) / sizeof(pci_known_devices[0])); ++i) {
        if (pci_known_devices[i].vendor == vendor &&
            pci_known_devices[i].device == device) {
            return pci_known_devices[i].name;
        }
    }
    return "Unknown PCI device";
}

#endif
