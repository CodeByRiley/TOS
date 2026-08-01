/* src/intf/drivers/driver.h - minimal kernel device/driver model.
 *
 * A bus enumerator creates struct device records. Registered drivers match
 * and probe those records. The first successful probe owns the device.
 *
 * The first bus adapter is PCI. Keeping matching out of pci.c means PCI
 * remains responsible for discovery while individual drivers remain
 * responsible for hardware policy.
 *
 * Implementation: src/impl/kernel/drivers/driver.c.
 */
#ifndef DRIVER_H
#define DRIVER_H

#include "pci/pci.h"
#include <stdint.h>

enum device_bus {
    DEVICE_BUS_NONE = 0,
    DEVICE_BUS_PCI  = 1,
};

struct device;

struct driver {
    const char *name;
    enum device_bus bus;
    int (*match)(const struct device *device);
    int (*probe)(struct device *device);
};

struct device {
    enum device_bus bus;
    union {
        struct pci_device pci;
    } bus_info;

    const struct driver *driver;
    void *driver_data;
};

void driver_core_init(void);

/* Register a driver and immediately try it against existing unbound devices. */
int driver_register(const struct driver *driver);

/* Import the PCI scan results into the device model and bind matching drivers.
 * Idempotent: each PCI function is imported once. */
int driver_probe_pci_devices(void);

uint32_t driver_device_count(void);
const struct device *driver_device_at(uint32_t index);

#endif
