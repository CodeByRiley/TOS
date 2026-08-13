// kernel/devices/isa.c
#include <isa/isa.h>
#include <drivers/driver.h>
#include <utilities/log.h>

/* A table of standard legacy ISA devices.
 * In real life, ISA cards were configured via jumpers,
 * so we just check the most common defaults. */
struct isa_legacy_entry {
    uint16_t io_base;
    uint8_t  irq;
    const char *name;
};

static const struct isa_legacy_entry legacy_devices[] = {
    { 0x220, 5, "SoundBlaster 16" },  // Standard SB16
    { 0x240, 5, "SoundBlaster 16" },  // Alternate SB16
    { 0x330, 0, "MPU-401 MIDI"    },  // Standard MIDI port
    { 0x3F0, 6, "Primary ATA"     },  // Floppy / IDE
    { 0x2E8, 3, "COM4 Serial"     },
    { 0x2F8, 3, "COM2 Serial"     },
    // Add more as needed
};

void isa_probe_devices(void) {
		log_write("ISA: probing legacy devices...", KERNEL, LOG_INFO);
    uint32_t count = sizeof(legacy_devices) / sizeof(legacy_devices[0]);
    log_write_hex("ISA: found ", count, KERNEL, LOG_INFO);
    uint32_t imported = 0;

    for (uint32_t i = 0; i < count; i++) {
        // register the device at its standard I/O port
        if (driver_register_isa_device(legacy_devices[i].io_base,
                                       legacy_devices[i].irq) == 0) {
            imported++;
        }
    }

    log_write_hex("ISA: legacy devices imported =", imported, KERNEL, LOG_INFO);
}
