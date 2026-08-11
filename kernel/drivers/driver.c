/* kernel/drivers/driver.c - minimal device/driver registry. */
#include "drivers/driver.h"
#include "utilities/log.h"
#include <stdint.h>

#define DRIVER_MAX_DRIVERS 16
#define DRIVER_MAX_DEVICES 64

static const struct driver *drivers[DRIVER_MAX_DRIVERS];
static struct device devices[DRIVER_MAX_DEVICES];
static uint32_t driver_count;
static uint32_t device_count;
static int pci_devices_probed;

static int bind_device(struct device *device) {
    if (device->driver)
        return 1;

    log_write("DRIVER: binding device", KERNEL, LOG_INFO);
    for (uint32_t i = 0; i < driver_count; i++) {
        const struct driver *driver = drivers[i];
        if (driver->bus != device->bus)
            continue;
        if (!driver->match(device))
            continue;

        log_write_string("DRIVER: probing", driver->name, KERNEL, LOG_INFO);
        if (driver->probe(device) != 0) {
            log_write_string("DRIVER: probe failed", driver->name,
                             KERNEL, LOG_WARN);
            continue;
        }

        device->driver = driver;
        log_write_string("DRIVER: bound", driver->name, KERNEL, LOG_INFO);
        return 1;
    }

    /* Most enumerated devices will remain unbound until their drivers are
     * implemented. */
    return 0;
}

void driver_core_init(void) {
    driver_count = 0;
    device_count = 0;
    pci_devices_probed = 0;
}

int driver_register(const struct driver *driver) {
    if (!driver || !driver->name || !driver->match || !driver->probe)
        return -1;
    if (driver->bus == DEVICE_BUS_NONE)
        return -1;
    if (driver_count >= DRIVER_MAX_DRIVERS) {
        log_write("DRIVER: registry full", KERNEL, LOG_ERROR);
        return -1;
    }

    drivers[driver_count++] = driver;
    log_write_string("DRIVER: registered", driver->name, KERNEL, LOG_INFO);

    /* Registration order is deliberately flexible: a driver registered after
     * bus enumeration still gets a chance to claim every unbound device. */
    for (uint32_t i = 0; i < device_count; i++)
        bind_device(&devices[i]);

    return 0;
}

int driver_register_isa_device(uint16_t io_base, uint8_t irq) {
    if (device_count >= DRIVER_MAX_DEVICES) return -1;
    log_write("DRIVER: registering device", KERNEL, LOG_INFO);

    struct device *device = &devices[device_count];
    device->bus = DEVICE_BUS_ISA;
    device->driver = 0;
    device->driver_data = 0;
    device->bus_info.isa.io_base = io_base;
    device->bus_info.isa.irq = irq;

    device_count++;
    log_write_hex("DRIVER: device registered ", device_count, KERNEL, LOG_INFO);
    // Try to bind a driver to it immediately
    bind_device(device);
    return 0;
}

int driver_probe_pci_devices(void) {
    if (pci_devices_probed)
        return 0;
    pci_devices_probed = 1;

    uint32_t count = pci_device_count();
    uint32_t imported = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (device_count >= DRIVER_MAX_DEVICES) {
            log_write("DRIVER: device table full", KERNEL, LOG_ERROR);
            break;
        }

        struct device *device = &devices[device_count];
        device->bus = DEVICE_BUS_PCI;
        device->driver = 0;
        device->driver_data = 0;

        if (!pci_device_at(i, &device->bus_info.pci))
            continue;

        device_count++;
        imported++;
        bind_device(device);
    }

    log_write_hex("DRIVER: PCI devices imported =", imported,
                  KERNEL, LOG_INFO);
    return (int)imported;
}

uint32_t driver_device_count(void) {
    return device_count;
}

const struct device *driver_device_at(uint32_t index) {
    if (index >= device_count)
        return 0;
    return &devices[index];
}
