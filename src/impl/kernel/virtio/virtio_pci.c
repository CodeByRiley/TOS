/* src/impl/kernel/virtio/virtio_pci.c — virtio 1.1 transport over PCI.
 *
 * Generic device-class-agnostic bringup: cap parse → MMIO mapping →
 * feature negotiation → virtqueue setup. Device-specific drivers
 * (virtio_gpu.c, etc.) build on top by submitting descriptors and
 * harvesting used entries via virtq_submit / virtq_reap.
 *
 * BAR mappings get carved out of a fixed kernel VA slab at
 * VIRTIO_BAR_VBASE, 64 MiB per device. Plenty of room for the ~16 KiB
 * a virtio device actually needs.
 */
#include "virtio/virtio.h"
#include "pci/pci.h"
#include "memory/pmm.h"
#include "memory/hhdm.h"
#include "memory/vmm.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

#define VIRTIO_BAR_VBASE    0xFFFFE00200000000ULL
#define VIRTIO_BAR_SLOT     (64ULL * 1024 * 1024)

static uint32_t bar_slot_next = 0;

struct virtio_pci_cap_hdr {
    uint8_t  cap_vndr;     /* 0x09 */
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cfg_type;
    uint8_t  bar;
    uint8_t  pad[3];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed));

/* Map a contiguous physical MMIO range into a kernel virt slot. Round to
 * pages. Returns the kernel VA matching `phys` (preserves intra-page offset).
 * Pages are mapped uncacheable — virtio MMIO accesses must not be cached. */
static uint64_t map_mmio(uint64_t phys, uint64_t length) {
    uint64_t page_off = phys & 0xFFFULL;
    uint64_t page_phys = phys & ~0xFFFULL;
    uint64_t end_phys  = (phys + length + 0xFFFULL) & ~0xFFFULL;
    uint64_t pages     = (end_phys - page_phys) / 4096;

    uint64_t va_base = VIRTIO_BAR_VBASE + (uint64_t)bar_slot_next * VIRTIO_BAR_SLOT;
    bar_slot_next++;

    for (uint64_t i = 0; i < pages; i++) {
        vmm_map(va_base + i * 4096,
                page_phys + i * 4096,
                VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT);
    }
    return va_base + page_off;
}

int virtio_pci_init(void *pci_device_ptr, struct virtio_dev *out) {
    struct pci_device *d = (struct pci_device*)pci_device_ptr;
    memset(out, 0, sizeof(*out));
    out->pci = d;

    /* Walk all virtio (vendor-id 0x09) caps. Each describes a region inside
     * one of the device's BARs. */
    uint8_t off = (uint8_t)((pci_cfg_read16(d->addr, PCI_CFG_STATUS) & PCI_STATUS_CAP_LIST)
                            ? (pci_cfg_read8(d->addr, PCI_CFG_CAP_PTR) & 0xFC)
                            : 0);
    int hops = 0;
    while (off && hops++ < 48) {
        struct virtio_pci_cap_hdr c;
        uint32_t w0 = pci_cfg_read32(d->addr, off + 0);
        uint32_t w1 = pci_cfg_read32(d->addr, off + 4);
        uint32_t w2 = pci_cfg_read32(d->addr, off + 8);
        uint32_t w3 = pci_cfg_read32(d->addr, off + 12);
        c.cap_vndr = (uint8_t)(w0 & 0xFF);
        c.cap_next = (uint8_t)((w0 >> 8) & 0xFF);
        c.cap_len  = (uint8_t)((w0 >> 16) & 0xFF);
        c.cfg_type = (uint8_t)((w0 >> 24) & 0xFF);
        c.bar      = (uint8_t)(w1 & 0xFF);
        c.offset   = w2;
        c.length   = w3;

        if (c.cap_vndr == VIRTIO_PCI_CAP_ID && c.bar < 6 && d->bar[c.bar].valid) {
            uint64_t phys = d->bar[c.bar].base + c.offset;
            uint64_t va   = map_mmio(phys, c.length);

            switch (c.cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                out->common = (volatile struct virtio_pci_common_cfg*)va;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG: {
                /* Notify cap appends a u32 notify_off_multiplier after the
                 * standard cap header (offset 16 in the cap). */
                out->notify_base = (volatile uint8_t*)va;
                uint32_t mult = pci_cfg_read32(d->addr, off + 16);
                out->notify_off_multiplier = mult;
                break;
            }
            case VIRTIO_PCI_CAP_ISR_CFG:
                out->isr = (volatile uint8_t*)va;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                out->device_cfg = (volatile uint8_t*)va;
                break;
            default:
                break;
            }
        }

        uint8_t next = (uint8_t)((pci_cfg_read32(d->addr, off) >> 8) & 0xFC);
        if (next == off) break;
        off = next;
    }

    if (!out->common) {
        log_write("virtio: no common cfg cap", KERNEL, LOG_ERROR);
        return -1;
    }
    if (!out->notify_base) {
        log_write("virtio: no notify cfg cap", KERNEL, LOG_ERROR);
        return -1;
    }

    /* Enable MMIO + bus mastering. */
    pci_enable(d);

    out->num_queues = out->common->num_queues;
    log_write_hex("virtio: num_queues =", out->num_queues, KERNEL, LOG_INFO);
    return 0;
}

int virtio_negotiate(struct virtio_dev *dev, uint64_t wanted) {
    volatile struct virtio_pci_common_cfg *c = dev->common;

    /* Reset. */
    c->device_status = 0;
    while (c->device_status != 0) { /* spin until accepted */ }

    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    /* Read device features (low + high dwords). */
    c->device_feature_select = 0;
    uint64_t dev_feat = (uint64_t)c->device_feature;
    c->device_feature_select = 1;
    dev_feat |= ((uint64_t)c->device_feature) << 32;

    /* Always demand VERSION_1 (modern transport). */
    if (!(dev_feat & VIRTIO_F_VERSION_1)) {
        log_write("virtio: device is legacy-only", KERNEL, LOG_ERROR);
        c->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }

    uint64_t driver_feat = (dev_feat & wanted) | VIRTIO_F_VERSION_1;
    c->driver_feature_select = 0;
    c->driver_feature = (uint32_t)(driver_feat & 0xFFFFFFFFu);
    c->driver_feature_select = 1;
    c->driver_feature = (uint32_t)(driver_feat >> 32);

    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                     | VIRTIO_STATUS_FEATURES_OK;
    if (!(c->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        log_write("virtio: FEATURES_OK refused", KERNEL, LOG_ERROR);
        c->device_status |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    return 0;
}

void virtio_driver_ok(struct virtio_dev *dev) {
    dev->common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                               | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
}

int virtio_queue_setup(struct virtio_dev *dev, uint16_t qidx, struct virtq *vq) {
    volatile struct virtio_pci_common_cfg *c = dev->common;
    memset(vq, 0, sizeof(*vq));

    c->queue_select = qidx;
    uint16_t qsize = c->queue_size;
    if (qsize == 0) {
        log_write_hex("virtio: queue not available, idx =", qidx, KERNEL, LOG_ERROR);
        return -1;
    }
    /* Cap to 64: keeps each ring inside a single 4 KiB frame even with worst-
     * case alignment, and is plenty for control + display traffic. */
    if (qsize > 64) qsize = 64;
    c->queue_size = qsize;
    vq->qidx       = qidx;
    vq->qsize      = qsize;
    vq->notify_off = c->queue_notify_off;

    /* Allocate physical ring pages and access them through the HHDM. */
    vq->desc_page  = pmm_alloc_frame();
    vq->avail_page = pmm_alloc_frame();
    vq->used_page  = pmm_alloc_frame();
    if (!vq->desc_page || !vq->avail_page || !vq->used_page) {
        log_write("virtio: queue alloc failed", KERNEL, LOG_ERROR);
        return -1;
    }
    memset(phys_to_virt(vq->desc_page),  0, 4096);
    memset(phys_to_virt(vq->avail_page), 0, 4096);
    memset(phys_to_virt(vq->used_page),  0, 4096);

    vq->desc  = phys_to_virt(vq->desc_page);
    vq->avail = phys_to_virt(vq->avail_page);
    vq->used  = phys_to_virt(vq->used_page);
    vq->desc_phys  = vq->desc_page;
    vq->avail_phys = vq->avail_page;
    vq->used_phys  = vq->used_page;

    /* Build descriptor free list. We chain 0..qsize-1 via the 'next' field. */
    for (uint16_t i = 0; i < qsize; i++) {
        vq->desc[i].next = (uint16_t)(i + 1);
        vq->desc[i].flags = VIRTQ_DESC_F_NEXT;
    }
    vq->desc[qsize - 1].next  = 0;
    vq->desc[qsize - 1].flags = 0;
    vq->free_head     = 0;
    vq->num_free      = qsize;
    vq->last_used_idx = 0;

    c->queue_desc   = vq->desc_phys;
    c->queue_driver = vq->avail_phys;
    c->queue_device = vq->used_phys;
    /* Don't enable yet — caller calls virtio_queue_enable after DRIVER_OK is
     * about to be issued. */
    return 0;
}

void virtio_queue_enable(struct virtio_dev *dev, struct virtq *vq) {
    dev->common->queue_select = vq->qidx;
    dev->common->queue_enable = 1;
}

void virtio_queue_notify(struct virtio_dev *dev, struct virtq *vq) {
    /* Doorbell address = notify_base + queue_notify_off * notify_off_multiplier.
     * We write the queue index as a 16-bit value. */
    volatile uint16_t *door = (volatile uint16_t*)
        (dev->notify_base + (uint32_t)vq->notify_off * dev->notify_off_multiplier);
    *door = vq->qidx;
}

uint16_t virtq_alloc_desc(struct virtq *vq) {
    if (vq->num_free == 0) return 0xFFFF;
    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    vq->desc[idx].next  = 0;
    vq->desc[idx].flags = 0;
    return idx;
}

void virtq_free_desc(struct virtq *vq, uint16_t idx) {
    if (idx >= vq->qsize) return;
    vq->desc[idx].next  = vq->free_head;
    vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
    vq->free_head = idx;
    vq->num_free++;
}

void virtq_submit(struct virtq *vq, uint16_t head) {
    uint16_t slot = vq->avail->idx % vq->qsize;
    vq->avail->ring[slot] = head;
    /* Memory barrier: ring write must be visible before idx update. */
    __asm__ volatile ("mfence" ::: "memory");
    vq->avail->idx = vq->avail->idx + 1;
    __asm__ volatile ("mfence" ::: "memory");
}

int virtq_reap(struct virtq *vq, uint16_t *out_id, uint32_t *out_len) {
    if (vq->last_used_idx == vq->used->idx) return 0;
    uint16_t slot = vq->last_used_idx % vq->qsize;
    if (out_id)  *out_id  = (uint16_t)vq->used->ring[slot].id;
    if (out_len) *out_len = vq->used->ring[slot].len;
    vq->last_used_idx++;
    return 1;
}
