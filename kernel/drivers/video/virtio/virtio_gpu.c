/* kernel/virtio/virtio_gpu.c , virtio-gpu (2D scanout) driver.
 *
 * Owns the host-side resource id + scanout. framebuffer.c owns the
 * kernel-side pixel buffer; they cooperate via create_scanout_2d (attach
 * backing), resize_scanout_2d, and flush_rect (push pixels).
 *
 * Single global instance , the kernel only ever drives one GPU. Submits
 * commands on the controlq, polls for responses synchronously (no
 * interrupt path yet), and exposes display-resize events through
 * virtio_gpu_poll_display_event() so the kernel TTY can rebind on host
 * window resize.
 */
#include <drivers/video/virtio/virtio_gpu.h>
#include <drivers/video/virtio/virtio.h>
#include <pci/pci.h>
#include <memory/pmm.h>
#include <memory/hhdm.h>
#include <memory/vmm.h>
#include <utilities/log.h>
#include <utilities/string.h>
#include <stdint.h>

/* Single global instance. */
static struct virtio_dev vdev;
static struct virtq      controlq;
static struct virtio_gpu gpu_state;

/* Scratch request/response buffers. We do all I/O synchronously, so a single
 * page each is plenty , the largest commands are ATTACH_BACKING with a tail
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
} PACKED;

struct gpu_resp_display_info {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} PACKED;

struct gpu_resource_create_2d {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} PACKED;

struct gpu_resource_unref {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} PACKED;

struct gpu_set_scanout {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} PACKED;

struct gpu_resource_flush {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} PACKED;

struct gpu_transfer_to_host_2d {
    struct gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} PACKED;

struct gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} PACKED;

/* The driver submits one command at a time. Keeping this 16 KiB coalescing
 * workspace out of the boot/task stack avoids overflowing the small kernel
 * stacks during framebuffer attachment. */
static struct gpu_mem_entry backing_runs[1024];

struct gpu_attach_backing_hdr {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} PACKED;

static int submit_two_buf(uint32_t req_len, uint32_t resp_len) {
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

    uint16_t got_id = 0;
    uint32_t got_len = 0;

    /* Busy-wait for 1,000,000 iterations. QEMU processes virtio-gpu commands
     * almost instantly on an unloaded VM, so this loop usually exits in
     * under a microsecond. No sleeps, no yields, no 10ms delays! */
    for (uint32_t i = 0; i < 1000000; i++) {
        if (virtq_reap(&controlq, &got_id, &got_len)) goto done;
        __asm__ volatile ("pause");
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
    if (n_pages == 0) return -1;

    backing_runs[0].addr = page_phys[0];
    backing_runs[0].length = 4096;
    backing_runs[0].padding = 0;
    entries = 1;
    for (uint32_t i = 1; i < n_pages; i++) {
        if (page_phys[i] == backing_runs[entries - 1].addr
                          + backing_runs[entries - 1].length) {
            backing_runs[entries - 1].length += 4096;
        } else {
            if (entries >= 1024) {
                log_write("gpu: too many backing runs", KERNEL, LOG_ERROR);
                return -1;
            }
            backing_runs[entries].addr    = page_phys[i];
            backing_runs[entries].length  = 4096;
            backing_runs[entries].padding = 0;
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
    for (uint32_t i = 0; i < entries; i++) tail[i] = backing_runs[i];

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
    /* Queue 1 (cursorq) is optional , we leave it unconfigured. */
    virtio_queue_enable(&vdev, &controlq);

    /* Allocate physical scratch pages and access them through the HHDM. */
    scratch_req_phys  = pmm_alloc_frame();
    scratch_resp_phys = pmm_alloc_frame();
    if (!scratch_req_phys || !scratch_resp_phys) {
        log_write("gpu: scratch alloc failed", KERNEL, LOG_ERROR);
        return -1;
    }
    scratch_req  = phys_to_virt(scratch_req_phys);
    scratch_resp = phys_to_virt(scratch_resp_phys);

    virtio_driver_ok(&vdev);

    uint32_t w = 0, h = 0;
    if (do_get_display_info(&w, &h) != 0) return -1;
    gpu_state.scanout_w = w;
    gpu_state.scanout_h = h;
    gpu_state.resource_id = 0;
    gpu_state.resource_w = 0;
    gpu_state.resource_h = 0;
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

int virtio_gpu_create_scanout_2d(uint32_t resource_w, uint32_t resource_h,
                                 uint32_t scanout_w, uint32_t scanout_h,
                                 const uint64_t *page_phys, uint32_t n_pages) {
    if (!gpu_state.ready) return -1;
    if (resource_w == 0 || resource_h == 0 || scanout_w == 0 || scanout_h == 0)
        return -1;
    if (scanout_w > resource_w || scanout_h > resource_h) return -1;
    if ((uint64_t)n_pages * 4096 <
        (uint64_t)resource_w * (uint64_t)resource_h * 4) return -1;

    /* Tear down the previous resource if any. SET_SCANOUT with resource_id=0
     * detaches the scanout cleanly per spec; UNREF then drops the resource. */
    if (gpu_state.resource_id) {
        do_set_scanout(0, 0, 0, 0);
        do_resource_unref(gpu_state.resource_id);
        gpu_state.resource_id = 0;
        gpu_state.resource_w = 0;
        gpu_state.resource_h = 0;
    }

    uint32_t rid = 1;   /* virtio-gpu resource IDs are driver-assigned; 1 is fine. */
    if (do_resource_create_2d(rid, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
                              resource_w, resource_h) != 0) return -1;
    if (do_attach_backing(rid, page_phys, n_pages) != 0) {
        do_resource_unref(rid);
        return -1;
    }
    if (do_set_scanout(0, rid, scanout_w, scanout_h) != 0) {
        do_resource_unref(rid);
        return -1;
    }

    gpu_state.resource_id = rid;
    gpu_state.resource_w  = resource_w;
    gpu_state.resource_h  = resource_h;
    gpu_state.scanout_w   = scanout_w;
    gpu_state.scanout_h   = scanout_h;
    return 0;
}

int virtio_gpu_resize_scanout_2d(uint32_t w, uint32_t h) {
    if (!gpu_state.ready || !gpu_state.resource_id) return -1;
    if (w == 0 || h == 0 || w > gpu_state.resource_w || h > gpu_state.resource_h)
        return -1;
    if (do_set_scanout(0, gpu_state.resource_id, w, h) != 0) return -1;
    gpu_state.scanout_w = w;
    gpu_state.scanout_h = h;
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
        q->offset      = (uint64_t)y * (uint64_t)gpu_state.resource_w * 4 +
                         (uint64_t)x * 4;
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
     * caller is expected to resize its visible rectangle. */
    uint32_t w = 0, h = 0;
    if (do_get_display_info(&w, &h) == 0) {
        gpu_state.scanout_w = w;
        gpu_state.scanout_h = h;
    }
    return 1;
}
