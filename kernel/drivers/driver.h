/* kernel/drivers/driver.h - minimal kernel device/driver model.
 *
 * A bus enumerator creates struct device records. Registered drivers match
 * and probe those records. The first successful probe owns the device.
 *
 * The first bus adapter is PCI. Keeping matching out of pci.c means PCI
 * remains responsible for discovery while individual drivers remain
 * responsible for hardware policy.
 *
 * Implementation: kernel/drivers/driver.c.
 */
#ifndef DRIVER_H
#define DRIVER_H

#include <pci/pci.h>
#include <stdint.h>

enum device_bus {
    DEVICE_BUS_NONE = 0,
    DEVICE_BUS_PCI  = 1,
    DEVICE_BUS_ISA  = 2,
};

struct isa_device {
	u16 io_base;
	u16 irq;
};

struct device;

struct driver {
    const char *name;
    enum device_bus bus;
    int (*match)(const struct device *device);
    int (*probe)(struct device *device);

    /* Optional non-blocking maintenance pass. The driver core calls this
     * from one shared poll task for every bound device that provides it.
     *
     * Returns non-zero when the pass actually did something - a frame
     * received, a completion reaped. The poll task uses that to decide
     * whether to come straight back or sleep, so a driver that always
     * reports work will spin the CPU exactly as the loop used to. Report
     * honestly: routine bookkeeping that happens every pass is not work. */
    int (*poll)(struct device *device);
};

struct device {
    enum device_bus bus;
    union {
        struct pci_device pci;
        struct isa_device isa;
    } bus_info;

    int enabled;
    const struct driver *driver;
    void *driver_data;
};

/* Maximum number of driver snapshots to retain for debugging & panic use. */
#define DRIVER_SNAP_MAX      16
#define DRIVER_SNAP_NAME_MAX 32

struct driver_snap {
    char name[DRIVER_SNAP_NAME_MAX];
    int bus;
    int poll;
    int enabled;
    u32 bound_devices;
};

void driver_core_init(void);

/* Register a driver and immediately try it against existing unbound devices. */
int driver_register(const struct driver *driver);
int driver_register_isa_device(u16 io_base, u8 irq);

/* Import the PCI scan results into the device model and bind matching drivers.
 * Idempotent: each PCI function is imported once. */
int driver_probe_pci_devices(void);

u32 driver_device_count(void);

const struct device *driver_device_at(u32 index);

/* Copy registered driver rows for panic/debug reporting. Allocation-free and
 * safe to call from fatal paths that can tolerate a best-effort snapshot. */
int driver_snapshot(struct driver_snap *out, int max);

#endif
