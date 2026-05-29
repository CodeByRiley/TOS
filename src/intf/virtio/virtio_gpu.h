#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

/* virtio-gpu device ID (PCI vendor 0x1AF4 transitional, 0x1AF4/0x1050+ modern).
 * QEMU exposes virtio-gpu / virtio-vga as device ID 0x1050. */
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

/* 2D pixel formats. R8G8B8X8 = 0x00RRGGBB layout matches our existing
 * framebuffer (low byte = blue). The spec uses backwards-looking names but
 * the bit layout matches what we want. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM        2
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM        1
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM        4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM        67

/* Event bits in device_cfg.events_read. */
#define VIRTIO_GPU_EVENT_DISPLAY                (1u << 0)

#define VIRTIO_GPU_MAX_SCANOUTS                 16

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_config {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
} __attribute__((packed));

/* Public driver state shared with framebuffer.c. The GPU owns the host-side
 * resource; the framebuffer module owns the kernel-side pixel buffer. */
struct virtio_gpu {
    int      ready;
    uint32_t resource_id;        /* current scanout-resource id, 0 if none */
    uint32_t scanout_w;
    uint32_t scanout_h;
};

/* Probe + bring up the device. Returns 0 on success. After this:
 *   - virtio_gpu_get_dims(&w, &h) returns the current scanout size
 *   - caller should allocate a w*h*4 pixel buffer and call set_scanout
 *
 * Pixel format is fixed at B8G8R8X8 (0xXXRRGGBB stored little-endian = the
 * existing framebuffer.c layout). */
int  virtio_gpu_init(void);

int  virtio_gpu_get_dims(uint32_t *w, uint32_t *h);

/* Replace the scanout's backing with the supplied scatter-gather list of
 * physical pages. n_pages * 4096 must be >= w*h*4. Drops/recreates the
 * resource id internally. Returns 0 on success. */
int  virtio_gpu_set_scanout_2d(uint32_t w, uint32_t h,
                               const uint64_t *page_phys, uint32_t n_pages);

/* Push the contents of (x, y, w, h) on the kernel-side pixel buffer to the
 * host scanout. Wraps TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. */
int  virtio_gpu_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Poll the device's events_read register. If VIRTIO_GPU_EVENT_DISPLAY is
 * pending, acknowledges it and returns 1; the caller should then call
 * virtio_gpu_get_dims to discover the new size. Returns 0 if no event. */
int  virtio_gpu_poll_display_event(void);

#endif
