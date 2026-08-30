/* kernel/virtio/virtio_gpu.h , virtio-gpu (2D scanout) driver.
 *
 * QEMU exposes virtio-gpu as PCI vendor 0x1AF4 device 0x1050. This driver
 * owns the host-side resource; framebuffer.c owns the kernel-side pixel
 * buffer. They cooperate via virtio_gpu_create_scanout_2d (attach backing),
 * virtio_gpu_resize_scanout_2d, and virtio_gpu_flush_rect (push pixels).
 *
 * Pixel format is fixed at B8G8R8X8 (0xXXRRGGBB stored little-endian
 * matches the existing framebuffer layout).
 *
 * Implementation: kernel/virtio/virtio_gpu.c.
 */
#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H


/* PACKED and friends. */
#include <utilities/types.h>
#include <stdint.h>

#define VIRTIO_PCI_VENDOR        0x1AF4
#define VIRTIO_GPU_DEVICE_ID     0x1050

/* virtio-gpu spec command types. */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_EDID                 0x010A

/* Response codes. */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104

/* 2D pixel formats. The spec uses backwards-looking names but the bit
 * layouts match what we want. R8G8B8X8 = 0x00RRGGBB matches our existing
 * framebuffer (low byte = blue). */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM        2
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM        1
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM        4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM        67

/* Event bits in device_cfg.events_read. */
#define VIRTIO_GPU_EVENT_DISPLAY                (1u << 0)

#define VIRTIO_GPU_MAX_SCANOUTS                 16

struct virtio_gpu_rect {
    u32 x, y, width, height;
} PACKED;

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    u32 enabled;
    u32 flags;
} PACKED;

struct virtio_gpu_config {
    u32 events_read;
    u32 events_clear;
    u32 num_scanouts;
    u32 num_capsets;
} PACKED;

/* Public driver state shared with framebuffer.c. */
struct virtio_gpu {
    int      ready;
    u32 resource_id;        /* current scanout-resource id, 0 if none */
    u32 resource_w;
    u32 resource_h;
    u32 scanout_w;
    u32 scanout_h;
};

/* Probe + bring up the device. Returns 0 on success. After this:
 *   - virtio_gpu_get_dims(&w, &h) returns the current scanout size
 *   - caller should allocate a w*h*4 pixel buffer and call set_scanout */
int  virtio_gpu_init(void);

int  virtio_gpu_get_dims(u32 *w, u32 *h);

/* Create one backed resource and display its top-left scanout_w x scanout_h
 * rectangle. Keeping the resource larger than the current scanout lets a
 * later host resize use only SET_SCANOUT instead of recreating the resource. */
int  virtio_gpu_create_scanout_2d(u32 resource_w, u32 resource_h,
                                  u32 scanout_w, u32 scanout_h,
                                  const u64 *page_phys, u32 n_pages);

/* Change the visible rectangle of the existing resource. The requested size
 * must fit inside resource_w x resource_h. */
int  virtio_gpu_resize_scanout_2d(u32 w, u32 h);

/* Push (x, y, w, h) from the kernel-side pixel buffer to the host
 * scanout. Wraps TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. */
int  virtio_gpu_flush_rect(u32 x, u32 y, u32 w, u32 h);

/* Poll device events. If VIRTIO_GPU_EVENT_DISPLAY is pending, ack it and
 * return 1; the caller should then call virtio_gpu_get_dims to discover
 * the new size. Returns 0 if no event. */
int  virtio_gpu_poll_display_event(void);

#endif
