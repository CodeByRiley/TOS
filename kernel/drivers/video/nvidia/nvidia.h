/* kernel/drivers/video/nvidia.h - NVIDIA PCI display probe.
 *
 * This first stage discovers NVIDIA display controllers and relates the
 * firmware-provided boot framebuffer to the GPU's PCI apertures. It does not
 * perform native modesetting or GPU acceleration yet.
 *
 * Implementation: kernel/drivers/video/nvidia.c.
 */
#ifndef NVIDIA_H
#define NVIDIA_H

#define NVIDIA_PCI_VENDOR_ID 0x10DE

int nvidia_driver_register(void);

/* Number of NVIDIA display controllers this driver has bound. Presence
 * only , a bound device is not necessarily driving the display. */
int nvidia_device_count(void);

/* Non-zero once a bound device has actually taken over scanout. Until
 * native modesetting exists this is always 0, so display selection must
 * fall through to whatever other adapter is available. Callers deciding
 * who owns the framebuffer want this, never nvidia_device_count(). */
int nvidia_display_active(void);

/* Called after the root filesystem is mounted.  PCI probe cannot load GSP
 * firmware because the FAT module is not available at probe time. */
void nvidia_driver_late_init(void);

#endif
