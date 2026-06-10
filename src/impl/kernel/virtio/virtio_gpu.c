/* src/impl/kernel/virtio/virtio_gpu.c — virtio-gpu (2D scanout) driver.
 *
 * Owns the host-side resource id + scanout. framebuffer.c owns the
 * kernel-side pixel buffer; they cooperate via set_scanout_2d (attach
 * backing) and flush_rect (push pixels).
 *
 * Single global instance — the kernel only ever drives one GPU. Submits
 * commands on the controlq, polls for responses synchronously (no
 * interrupt path yet), and exposes display-resize events through
 * virtio_gpu_poll_display_event() so the kernel TTY can rebind on host
 * window resize.
 */
#include "virtio/virtio_gpu.h"
#include "virtio/virtio.h"
#include "pci/pci.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "sched/sched.h"
#include "utilities/log.h"
#include "utilities/string.h"
#include <stdint.h>

/* Single global instance. */
static struct virtio_dev vdev;
static struct virtq      controlq;
static struct virtio_gpu gpu_state;

/* Scratch request/response buffers. We do all I/O synchronously, so a single
 * page each is plenty — the largest commands are ATTACH_BACKING with a tail
 * of mem_entry records, which still fit in 4 KiB up to ~340 entries (each is
 * 16 bytes). For framebuffers larger than 1.3 MiB we'll split the attach
 * across multiple commands. */
static uint64_t scratch_req_phys;
static uint64_t scratch_resp_phys;
static uint8_t *scratch_req;
static uint8_t *scratch_resp;

/* virtio-gpu wire structures (subset we use). */
struct gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct gpu_resp_display_info {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

struct gpu_resource_create_2d {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct gpu_resource_unref {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct gpu_set_scanout {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct gpu_resource_flush {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct gpu_transfer_to_host_2d {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct gpu_attach_backing_hdr {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

static int submit_two_buf(uint32_t req_len, uint32_t resp_len) {
    /* Build a 2-descriptor chain: req (RO), resp (WO). Reap synchronously. */
    uint16_t d0 = virtq_alloc_desc(&controlq);
    uint16_t d1 = virtq_alloc_desc(&controlq);
    if (d0 == 0xFFFF || d1 == 0xFFFF) {
        log_write("gpu: no free descs", KERNEL, LOG_ERROR);
        return -1;
    }

    controlq.desc[d0].addr  = scratch_req_phys;
    controlq.desc[d0].len   = req_len;
    controlq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    controlq.desc[d0].next  = d1;

    controlq.desc[d1].addr  = scratch_resp_phys;
    controlq.desc[d1].len   = resp_len;
    controlq.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    controlq.desc[d1].next  = 0;

    virtq_submit(&controlq, d0);
    virtio_queue_notify(&vdev, &controlq);

    /* Wait for the used ring to advance. Fast path: pause-spin for ~1024
     * cycles in case the device replies immediately (host usually does on
     * an unloaded VM). Slow path: yield to the scheduler each iteration so
     * we don't monopolise the CPU while waiting for a sluggish host. We
     * cap total wait at ~10000 yields so a buggy host can't lock us. */
    uint16_t got_id = 0;
    uint32_t got_len = 0;

    for (uint32_t i = 0; i < 1024; i++) {
        if (virtq_reap(&controlq, &got_id, &got_len)) goto done;
        __asm__ volatile ("pause");
    }
    for (uint32_t i = 0; i < 10000u; i++) {
        if (virtq_reap(&controlq, &got_id, &got_len)) goto done;
        task_yield();
    }
    log_write("gpu: command timed out", KERNEL, LOG_ERROR);
    return -1;
done:
    virtq_free_desc(&controlq, d0);
    virtq_free_desc(&controlq, d1);
    return 0;
}

/* Issue a command whose request body lives in scratch_req and whose response
 * goes to scratch_resp. Returns response type, or 0 on failure. */
static uint32_t do_cmd(uint32_t req_len, uint32_t resp_len) {
    if (submit_two_buf(req_len, resp_len) != 0) return 0;
    struct gpu_ctrl_hdr *resp = (struct gpu_ctrl_hdr*)scratch_resp;
    return resp->type;
}

static int do_get_display_info(uint32_t *w, uint32_t *h) {
    memset(scratch_req,  0, sizeof(struct gpu_ctrl_hdr));
    memset(scratch_resp, 0, sizeof(struct gpu_resp_display_info));
    struct gpu_ctrl_hdr *h_req = (struct gpu_ctrl_hdr*)scratch_req;
    h_req->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    uint32_t resp_type = do_cmd(sizeof(*h_req), sizeof(struct gpu_resp_display_info));
    if (resp_type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        log_write_hex("gpu: display_info bad resp =", resp_type, KERNEL, LOG_ERROR);
        return -1;
    }
    struct gpu_resp_display_info *r = (struct gpu_resp_display_info*)scratch_resp;
    /* Find the first enabled scanout. Most QEMU configs put it at index 0. */
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (r->pmodes[i].enabled) {
            *w = r->pmodes[i].r.width;
            *h = r->pmodes[i].r.height;
            return 0;
        }
    }
    log_write("gpu: no enabled scanouts", KERNEL, LOG_ERROR);
    return -1;
}

static int do_resource_create_2d(uint32_t rid, uint32_t format,
                                 uint32_t w, uint32_t h) {
    memset(scratch_req,  0, sizeof(struct gpu_resource_create_2d));
    memset(scratch_resp, 0, sizeof(struct gpu_ctrl_hdr));
    struct gpu_resource_create_2d *q = (struct gpu_resource_create_2d*)scratch_req;
    q->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    q->resource_id = rid;
    q->format      = format;
    q->width       = w;
    q->height      = h;
    uint32_t t = do_cmd(sizeof(*q), sizeof(struct gpu_ctrl_hdr));
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        log_write_hex("gpu: create_2d bad resp =", t, KERNEL, LOG_ERROR);
        return -1;
    }
    return 0;
}

static int do_resource_unref(uint32_t rid) {
    memset(scratch_req,  0, sizeof(struct gpu_resource_unref));
    memset(scratch_resp, 0, sizeof(struct gpu_ctrl_hdr));
    struct gpu_resource_unref *q = (struct gpu_resource_unref*)scratch_req;
    q->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    q->resource_id = rid;
    uint32_t t = do_cmd(sizeof(*q), sizeof(struct gpu_ctrl_hdr));
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        log_write_hex("gpu: unref bad resp =", t, KERNEL, LOG_ERROR);
        return -1;
    }
    return 0;
}

static int do_attach_backing(uint32_t rid,
                             const uint64_t *page_phys, uint32_t n_pages) {
    /* Layout: [hdr][nr_entries x mem_entry]. Whole thing into scratch_req. */
    const uint32_t max_entries =
        (4096 - sizeof(struct gpu_attach_backing_hdr)) /
        sizeof(struct gpu_mem_entry);

    /* Coalesce adjacent pages: contiguous runs of identity-mapped frames are
     * the common case (pmm allocs sequentially when memory is fresh), and the
     * spec allows arbitrary entry length. Coalescing keeps us under the entry
     * cap for large framebuffers. */
    uint32_t entries = 0;
    struct gpu_mem_entry tmp[1024];
    if (n_pages == 0) return -1;

    tmp[0].addr = page_phys[0];
    tmp[0].length = 4096;
    tmp[0].padding = 0;
    entries = 1;
    for (uint32_t i = 1; i < n_pages; i++) {
        if (page_phys[i] == tmp[entries - 1].addr + tmp[entries - 1].length) {
            tmp[entries - 1].length += 4096;
        } else {
            if (entries >= 1024) {
                log_write("gpu: too many backing runs", KERNEL, LOG_ERROR);
                return -1;
            }
            tmp[entries].addr    = page_phys[i];
            tmp[entries].length  = 4096;
            tmp[entries].padding = 0;
            entries++;
        }
    }

    if (entries > max_entries) {
        log_write_hex("gpu: backing entries overflow =", entries, KERNEL, LOG_ERROR);
        return -1;
    }

    struct gpu_attach_backing_hdr *q = (struct gpu_attach_backing_hdr*)scratch_req;
    memset(q, 0, sizeof(*q));
    q->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    q->resource_id = rid;
    q->nr_entries  = entries;

    struct gpu_mem_entry *tail = (struct gpu_mem_entry*)(scratch_req + sizeof(*q));
    for (uint32_t i = 0; i < entries; i++) tail[i] = tmp[i];

    uint32_t req_len = (uint32_t)sizeof(*q) + entries * (uint32_t)sizeof(struct gpu_mem_entry);
    memset(scratch_resp, 0, sizeof(struct gpu_ctrl_hdr));
    uint32_t t = do_cmd(req_len, sizeof(struct gpu_ctrl_hdr));
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        log_write_hex("gpu: attach_backing bad resp =", t, KERNEL, LOG_ERROR);
        return -1;
    }
    return 0;
}

static int do_set_scanout(uint32_t scanout_id, uint32_t rid,
                          uint32_t w, uint32_t h) {
    memset(scratch_req,  0, sizeof(struct gpu_set_scanout));
    memset(scratch_resp, 0, sizeof(struct gpu_ctrl_hdr));
    struct gpu_set_scanout *q = (struct gpu_set_scanout*)scratch_req;
    q->hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    q->r.x         = 0;
    q->r.y         = 0;
    q->r.width     = w;
    q->r.height    = h;
    q->scanout_id  = scanout_id;
    q->resource_id = rid;
    uint32_t t = do_cmd(sizeof(*q), sizeof(struct gpu_ctrl_hdr));
    if (t != VIRTIO_GPU_RESP_OK_NODATA) {
        log_write_hex("gpu: set_scanout bad resp =", t, KERNEL, LOG_ERROR);
        return -1;
    }
    return 0;
}

int virtio_gpu_init(void) {
    struct pci_device dev;
    if (!pci_find_by_id(VIRTIO_PCI_VENDOR, VIRTIO_GPU_DEVICE_ID, &dev)) {
        log_write("gpu: virtio-gpu PCI device not found", KERNEL, LOG_ERROR);
        return -1;
    }

    if (virtio_pci_init(&dev, &vdev) != 0) return -1;

    /* No optional features required for our minimal usage. Negotiate empty
     * set (just VERSION_1, which virtio_negotiate ORs in unconditionally). */
    if (virtio_negotiate(&vdev, 0) != 0) return -1;

    if (virtio_queue_setup(&vdev, 0, &controlq) != 0) return -1;
    /* Queue 1 (cursorq) is optional — we leave it unconfigured. */
    virtio_queue_enable(&vdev, &controlq);

    /* Allocate scratch req/resp pages. Identity-mapped, accessible via phys. */
    scratch_req_phys  = pmm_alloc_frame();
    scratch_resp_phys = pmm_alloc_frame();
    if (!scratch_req_phys || !scratch_resp_phys) {
        log_write("gpu: scratch alloc failed", KERNEL, LOG_ERROR);
        return -1;
    }
    scratch_req  = (uint8_t*)scratch_req_phys;
    scratch_resp = (uint8_t*)scratch_resp_phys;

    virtio_driver_ok(&vdev);

    uint32_t w = 0, h = 0;
    if (do_get_display_info(&w, &h) != 0) return -1;
    gpu_state.scanout_w = w;
    gpu_state.scanout_h = h;
    gpu_state.resource_id = 0;
    gpu_state.ready = 1;
    log_write_hex("gpu: scanout w =", w, KERNEL, LOG_INFO);
    log_write_hex("gpu: scanout h =", h, KERNEL, LOG_INFO);
    return 0;
}

int virtio_gpu_get_dims(uint32_t *w, uint32_t *h) {
    if (!gpu_state.ready) return -1;
    *w = gpu_state.scanout_w;
    *h = gpu_state.scanout_h;
    return 0;
}

int virtio_gpu_set_scanout_2d(uint32_t w, uint32_t h,
                              const uint64_t *page_phys, uint32_t n_pages) {
    if (!gpu_state.ready) return -1;

    /* Tear down the previous resource if any. SET_SCANOUT with resource_id=0
     * detaches the scanout cleanly per spec; UNREF then drops the resource. */
    if (gpu_state.resource_id) {
        do_set_scanout(0, 0, w, h);
        do_resource_unref(gpu_state.resource_id);
        gpu_state.resource_id = 0;
    }

    uint32_t rid = 1;   /* virtio-gpu resource IDs are driver-assigned; 1 is fine. */
    if (do_resource_create_2d(rid, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, w, h) != 0) return -1;
    if (do_attach_backing(rid, page_phys, n_pages) != 0) return -1;
    if (do_set_scanout(0, rid, w, h) != 0) return -1;

    gpu_state.resource_id = rid;
    gpu_state.scanout_w   = w;
    gpu_state.scanout_h   = h;
    return 0;
}

int virtio_gpu_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!gpu_state.ready || !gpu_state.resource_id) return -1;

    /* TRANSFER_TO_HOST_2D: copy guest-side pixels into the host resource. */
    {
        memset(scratch_req,  0, sizeof(struct gpu_transfer_to_host_2d));
        memset(scratch_resp, 0, sizeof(struct gpu_ctrl_hdr));
        struct gpu_transfer_to_host_2d *q = (struct gpu_transfer_to_host_2d*)scratch_req;
        q->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        q->r.x         = x;
        q->r.y         = y;
        q->r.width     = w;
        q->r.height    = h;
        q->offset      = (uint64_t)y * (uint64_t)gpu_state.scanout_w * 4 + (uint64_t)x * 4;
        q->resource_id = gpu_state.resource_id;
        uint32_t t = do_cmd(sizeof(*q), sizeof(struct gpu_ctrl_hdr));
        if (t != VIRTIO_GPU_RESP_OK_NODATA) {
            log_write_hex("gpu: xfer2d bad resp =", t, KERNEL, LOG_ERROR);
            return -1;
        }
    }
    /* RESOURCE_FLUSH: tell the host to actually show what we just transferred. */
    {
        memset(scratch_req,  0, sizeof(struct gpu_resource_flush));
        memset(scratch_resp, 0, sizeof(struct gpu_ctrl_hdr));
        struct gpu_resource_flush *q = (struct gpu_resource_flush*)scratch_req;
        q->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        q->r.x         = x;
        q->r.y         = y;
        q->r.width     = w;
        q->r.height    = h;
        q->resource_id = gpu_state.resource_id;
        uint32_t t = do_cmd(sizeof(*q), sizeof(struct gpu_ctrl_hdr));
        if (t != VIRTIO_GPU_RESP_OK_NODATA) {
            log_write_hex("gpu: flush bad resp =", t, KERNEL, LOG_ERROR);
            return -1;
        }
    }
    return 0;
}

int virtio_gpu_poll_display_event(void) {
    if (!gpu_state.ready) return 0;
    volatile struct virtio_gpu_config *cfg =
        (volatile struct virtio_gpu_config*)vdev.device_cfg;
    uint32_t ev = cfg->events_read;
    if (!(ev & VIRTIO_GPU_EVENT_DISPLAY)) return 0;
    /* Ack: write the same bits to events_clear. */
    cfg->events_clear = VIRTIO_GPU_EVENT_DISPLAY;

    /* Re-read display info so subsequent virtio_gpu_get_dims reflects the
     * new size. The actual scanout still has the old resource attached;
     * caller is expected to virtio_gpu_set_scanout_2d with the new size. */
    uint32_t w = 0, h = 0;
    if (do_get_display_info(&w, &h) == 0) {
        gpu_state.scanout_w = w;
        gpu_state.scanout_h = h;
    }
    return 1;
}
