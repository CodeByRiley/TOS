/* kernel/drivers/driver.c - minimal device/driver registry. */
#include <drivers/driver.h>
#include <sched/sched.h>
#include <stddef.h>
#include <stdint.h>
#include <utilities/log.h>

#define DRIVER_MAX_DRIVERS 16
#define DRIVER_MAX_DEVICES 64

static const struct driver *drivers[DRIVER_MAX_DRIVERS];
static struct device devices[DRIVER_MAX_DEVICES];
static u32 driver_count;
static u32 device_count;
static int pci_devices_probed;
static int poll_task_started;

/* The poll task used to end each pass with task_yield(). That looks like
 * it gives up the CPU, but yield returns immediately when nothing else is
 * runnable, so the loop became a tight spin that burned a core. Under TCG
 * it starved the guest badly enough to drop PIT interrupts: usleep(1)
 * measured 15.7 seconds, and every sleeping task in the system inherited
 * that error.
 *
 * So sleep when there is nothing to do, and only come straight back when
 * the last pass actually found work -- a receive burst still drains at
 * full speed, because each pass that handles a frame yields rather than
 * sleeps. Idle costs one wakeup per tick instead of a whole core.
 *
 * This is a stopgap for the receive path specifically. The real answer is
 * the e1000 interrupt handler, which is on the TODO; polling is honest
 * while the protocol layers above it are still the unknown quantity. */
static void driver_poll_thread(void) {
  int busy = 0;
  for (;;) {
    int worked = 0;

    for (u32 i = 0; i < device_count; i++) {
      struct device *device = &devices[i];
      if (device->driver && device->driver->poll) {
        worked |= device->driver->poll(device);
      }
    }

    if (worked && busy < 8) {
      busy++;
      task_yield();
    } else {
      busy = 0;
      task_sleep_ticks(1);
    }
  }
}

static void ensure_poll_task(void) {
  if (poll_task_started)
    return;

  struct task *task = task_spawn(driver_poll_thread);
  if (!task) {
    log_write("DRIVER: could not spawn poll task", KERNEL, LOG_WARN);
    return;
  }

  task_set_name(task, "drivers");
  poll_task_started = 1;
  log_write("DRIVER: poll task spawned", KERNEL, LOG_INFO);
}

static int bind_device(struct device *device) {
  if (device->driver)
    return 1;

  for (u32 i = 0; i < driver_count; i++) {
    const struct driver *driver = drivers[i];
    if (driver->bus != device->bus)
      continue;
    if (!driver->match(device))
      continue;

    log_write_string("DRIVER: probing", driver->name, KERNEL, LOG_INFO);

    if (driver->probe(device) != 0) {
      log_write_string("DRIVER: probe failed", driver->name, KERNEL, LOG_WARN);
      continue;
    }
    log_write_string("DRIVER: probe success", driver->name, KERNEL, LOG_INFO);
    device->driver = driver;
    /* A successful probe means the device is operational. Keep this
     * explicit so diagnostics can distinguish a registered driver from
     * one that actually owns working hardware. */
    device->enabled = 1;
    if (driver->poll)
      ensure_poll_task();
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
  poll_task_started = 0;
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
  for (u32 i = 0; i < device_count; i++)
    bind_device(&devices[i]);

  return 0;
}

int driver_register_isa_device(u16 io_base, u8 irq) {
  if (device_count >= DRIVER_MAX_DEVICES)
    return -1;
  log_write("DRIVER: registering device", KERNEL, LOG_INFO);

  struct device *device = &devices[device_count];
  device->bus = DEVICE_BUS_ISA;
  device->enabled = 0;
  device->driver = 0;
  device->driver_data = 0;
  device->bus_info.isa.io_base = io_base;
  device->bus_info.isa.irq = irq;

  device_count++;
  log_write_hex("DRIVER: device registered ", device_count, KERNEL, LOG_INFO);
  bind_device(device);
  return 0;
}

int driver_probe_pci_devices(void) {
  u32 count = pci_device_count();
  if (count == 0)
    return 0; /* not enumerated yet, or genuinely empty */
  if (pci_devices_probed)
    return 0;
  pci_devices_probed = 1;
  u32 imported = 0;

  for (u32 i = 0; i < count; i++) {
    if (device_count >= DRIVER_MAX_DEVICES) {
      log_write("DRIVER: device table full", KERNEL, LOG_ERROR);
      break;
    }

    struct device *device = &devices[device_count];
    if (!pci_device_at(i, &device->bus_info.pci))
      continue;

    device->bus = DEVICE_BUS_PCI;
    device->enabled = 0;
    device->driver = 0;
    device->driver_data = 0;

    device_count++;
    imported++;
    bind_device(device);
  }

  log_write_hex("DRIVER: PCI devices imported =", imported, KERNEL, LOG_INFO);
  return (int)imported;
}

u32 driver_device_count(void) { return device_count; }

const struct device *driver_device_at(u32 index) {
  if (index >= device_count)
    return 0;
  return &devices[index];
}

int driver_snapshot(struct driver_snap *out, int max) {
  if (!out || max <= 0)
    return 0;

  int n = 0;
  for (u32 i = 0; i < driver_count && n < max; i++) {
    const struct driver *drv = drivers[i];
    if (!drv)
      continue;

    struct driver_snap *s = &out[n];
    s->bus = (int)drv->bus;
    s->poll = drv->poll ? 1 : 0;
    s->bound_devices = 0;
    s->enabled = 0;

    const char *name = drv->name ? drv->name : "unnamed";
    usize k = 0;
    while (k + 1 < DRIVER_SNAP_NAME_MAX && name[k]) {
      s->name[k] = name[k];
      k++;
    }
    s->name[k] = '\0';

    for (u32 j = 0; j < device_count; j++) {
      const struct device *d = &devices[j];
      if (d->driver == drv) {
        s->bound_devices++;
        if (d->enabled)
          s->enabled = 1;
      }
    }
    n++;
  }

  return n;
}
