/* kernel/virtio/virtio.h , virtio 1.1 modern transport.
 *
 * Common PCI capability layouts, device-status bits, and virtqueue
 * structures shared across all virtio device classes. Device-specific
 * headers (virtio_gpu.h, etc.) build on this surface.
 *
 * We refuse to talk to anything that doesn't advertise
 * VIRTIO_F_VERSION_1 , no transitional / legacy mode.
 *
 * Implementations:
 *   - virtio_pci_init / virtio_negotiate / queue setup: kernel/virtio/virtio_pci.c
 *   - device-specific drivers:                          kernel/virtio/virtio_*.c
 */
#ifndef VIRTIO_H
#define VIRTIO_H


/* PACKED and friends. */
#include <utilities/types.h>
#include <stdint.h>

/* virtio 1.1, section 4.1.4 , vendor-specific PCI capability structure. */
#define VIRTIO_PCI_CAP_ID            0x09
#define VIRTIO_PCI_CAP_COMMON_CFG    1
#define VIRTIO_PCI_CAP_NOTIFY_CFG    2
#define VIRTIO_PCI_CAP_ISR_CFG       3
#define VIRTIO_PCI_CAP_DEVICE_CFG    4
#define VIRTIO_PCI_CAP_PCI_CFG       5

/* Device Status (common cfg, offset 0x14) */
//
// Bit | Name        | Description
// 7   | FAILED      | Driver gave up; device is unusable until reset
// 6   | NEEDS_RESET | Device-set: it hit an unrecoverable error
// 5-4 | Reserved    |
// 3   | FEATURES_OK | Driver finished feature negotiation
// 2   | DRIVER_OK   | Driver is live; the device may start using queues
// 1   | DRIVER      | Driver knows how to drive this device
// 0   | ACKNOWLEDGE | Driver noticed the device
//
// These are cumulative and strictly ordered , writing 0 resets, then the bits
// are OR'd in the order listed. Two checks matter: after setting FEATURES_OK
// the driver must read the byte back, since the device clears the bit to veto
// the feature set; and queues must be configured before DRIVER_OK, because
// the device may begin using them the moment it is set.
#define VIRTIO_STATUS_RESET           0
#define VIRTIO_STATUS_ACKNOWLEDGE     1
#define VIRTIO_STATUS_DRIVER          2
#define VIRTIO_STATUS_DRIVER_OK       4
#define VIRTIO_STATUS_FEATURES_OK     8
#define VIRTIO_STATUS_NEEDS_RESET     64
#define VIRTIO_STATUS_FAILED          128

/* Transport feature bit 32 , device understands the modern (v1.0+)
 * layout. We refuse to talk to anything that doesn't set this. */
#define VIRTIO_F_VERSION_1            (1ULL << 32)

/* Virtqueue descriptor flags. */
#define VIRTQ_DESC_F_NEXT      1
#define VIRTQ_DESC_F_WRITE     2
#define VIRTQ_DESC_F_INDIRECT  4

/* Avail/used ring flags. */
#define VIRTQ_AVAIL_F_NO_INTERRUPT  1
#define VIRTQ_USED_F_NO_NOTIFY      1

struct virtq_desc {
    u64 addr;     /* guest-physical */
    u32 len;
    u16 flags;
    u16 next;
} PACKED;

struct virtq_avail {
    u16 flags;
    u16 idx;
    u16 ring[];   /* size = qsize; followed by optional used_event u16 */
} PACKED;

struct virtq_used_elem {
    u32 id;
    u32 len;
} PACKED;

struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[];   /* size = qsize; followed by avail_event */
} PACKED;

/* virtio 1.1, 4.1.4.3 , common configuration structure. Field offsets
 * and widths are part of the spec; do not reorder. */
struct virtio_pci_common_cfg {
    u32 device_feature_select;     /* 0x00 RW */
    u32 device_feature;             /* 0x04 RO */
    u32 driver_feature_select;      /* 0x08 RW */
    u32 driver_feature;             /* 0x0C RW */
    u16 msix_config;                /* 0x10 RW */
    u16 num_queues;                 /* 0x12 RO */
    u8  device_status;              /* 0x14 RW */
    u8  config_generation;          /* 0x15 RO */

    u16 queue_select;               /* 0x16 RW */
    u16 queue_size;                 /* 0x18 RW */
    u16 queue_msix_vector;          /* 0x1A RW */
    u16 queue_enable;               /* 0x1C RW */
    u16 queue_notify_off;           /* 0x1E RO */
    u64 queue_desc;                 /* 0x20 RW */
    u64 queue_driver;               /* 0x28 RW */
    u64 queue_device;               /* 0x30 RW */
} PACKED;

/* Driver-side view of a virtio device after probe + cap parse. */
struct virtio_dev {
    void *pci;                          /* opaque pci_device*              */

    volatile struct virtio_pci_common_cfg *common;
    volatile u8  *isr;
    volatile u8  *notify_base;
    u32           notify_off_multiplier;
    volatile u8  *device_cfg;

    u16 num_queues;
};

/* One virtqueue + its backing pages. */
struct virtq {
    u16 qidx;
    u16 qsize;
    u16 notify_off;
    u16 free_head;
    u16 num_free;
    u16 last_used_idx;

    volatile struct virtq_desc  *desc;
    volatile struct virtq_avail *avail;
    volatile struct virtq_used  *used;

    u64 desc_phys;
    u64 avail_phys;
    u64 used_phys;

    /* Backing pages (one phys page per ring). */
    u64 desc_page;
    u64 avail_page;
    u64 used_page;
};

/* Probe a PCI device, parse virtio caps, map config regions, fill *out.
 * Returns 0 on success. */
int  virtio_pci_init(void *pci_device_ptr, struct virtio_dev *out);

/* Reset the device, set ACKNOWLEDGE | DRIVER, negotiate features. */
int  virtio_negotiate(struct virtio_dev *dev, u64 wanted);

/* Final DRIVER_OK after queues are set up. */
void virtio_driver_ok(struct virtio_dev *dev);

/* Allocate + register one virtqueue. qsize is requested; the device may
 * clamp down. */
int  virtio_queue_setup(struct virtio_dev *dev, u16 qidx, struct virtq *vq);

/* Mark the queue enabled in the device after virtio_queue_setup. */
void virtio_queue_enable(struct virtio_dev *dev, struct virtq *vq);

/* Kick: write the queue index to its notify doorbell. */
void virtio_queue_notify(struct virtio_dev *dev, struct virtq *vq);

/* Descriptor-pool helpers. virtq_alloc_desc returns desc index, or
 * 0xFFFF if empty. */
u16 virtq_alloc_desc(struct virtq *vq);
void     virtq_free_desc (struct virtq *vq, u16 idx);

/* Submit a single-buffer (or descriptor-chain head) to the avail ring. */
void     virtq_submit    (struct virtq *vq, u16 head);

/* Try to reap one used entry. Returns 1 if harvested into *out_id /
 * *out_len, 0 if the ring was empty. */
int      virtq_reap      (struct virtq *vq, u16 *out_id, u32 *out_len);

#endif
