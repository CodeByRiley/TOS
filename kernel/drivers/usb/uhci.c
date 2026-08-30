#include <drivers/usb/uhci.h>
#include <devices/io.h>
#include <utilities/log.h>
#include <utilities/types.h>
#include <devices/pit.h>
#include <devices/usb.h>
#include <input/mouse.h>
#include <interrupts/idt.h>
#include <interrupts/pic.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <pci/pci.h>
#include <sched/sched.h>
#include <utilities/string.h>
#include <stdint.h>

/* Link pointers are 32-bit, so every DMA structure has to live under 4 GiB. */
#define UHCI_DMA_LIMIT 0x100000000ULL

#define UHCI_NUM_PORTS 2

/* Every DMA structure lives in one 4 KiB frame: a single allocation is
 * contiguous, 16-byte aligned throughout, and satisfies UHCI_DMA_LIMIT once.
 *
 *   0x000  control queue head    (8 B)
 *   0x010  TD pool               (64 x 16 B)
 *   0x410  setup packet          (8 B)
 *   0x800  data buffer           (1 KiB)
 *   0xC00  interrupt queue head  (8 B)
 *   0xC10  interrupt TD          (16 B)
 *   0xC20  interrupt data buffer (64 B)
 */
#define UHCI_ARENA_QH       0x000
#define UHCI_ARENA_TD       0x010
#define UHCI_ARENA_NTD      64
#define UHCI_ARENA_SETUP    0x410
#define UHCI_ARENA_DATA     0x800
#define UHCI_ARENA_DATA_MAX 1024
#define UHCI_ARENA_INT_QH   0xC00
#define UHCI_ARENA_INT_TD   0xC10
#define UHCI_ARENA_INT_BUF  0xC20
#define UHCI_ARENA_INT_MAX  64

/* HID configurations run well under 64 bytes; truncating a larger one beats
 * overrunning the buffer. */
#define UHCI_CONFIG_BUF_MAX 256

/* Periodic reports/errors logged before going quiet. */
#define UHCI_INT_LOG_LIMIT 4

/* Wall-clock, not loop counts. The spec allows 5 s per standard request, but
 * a device that slow should not hold up the boot. */
#define UHCI_XFER_TIMEOUT_MS 500

/* Every device must accept 8 until it reports its real ep0 packet size. */
#define UHCI_EP0_DEFAULT_MAXPACKET 8

#define UHCI_INT_KIND_NONE           0
#define UHCI_INT_KIND_HID_BOOT_MOUSE 1
#define UHCI_INT_KIND_HID_TABLET     2

/* Hardware contract: a packing surprise would misalign the TD pool and look
 * like a device fault at runtime. Fail the build instead. */
_Static_assert(sizeof(struct uhci_td) == 16, "UHCI TD must be 16 bytes");
_Static_assert(sizeof(struct uhci_qh) == 8, "UHCI QH must be 8 bytes");
_Static_assert(sizeof(struct uhci_frame) == 4, "UHCI frame entry must be 4 bytes");
_Static_assert(sizeof(struct usb_setup_packet) == 8, "USB setup packet must be 8 bytes");
_Static_assert(sizeof(struct usb_descriptor) == 18, "USB device descriptor must be 18 bytes");

/* A periodic IN endpoint. Unlike a control chain this TD is never torn down:
 * the controller retries it every interval until the device answers, and the
 * completion handler re-arms it. One per controller suits a boot mouse or
 * tablet; a real stack would keep a list. */
struct uhci_int_ep {
  int active;

  u8  addr;
  u8  ep;
  int low_speed;
  u16 maxlen;
  u8  interval; /* bInterval, in frames */
  u8  kind;

  int toggle;

  struct uhci_qh *qh;
  u64 qh_phys;
  struct uhci_td *td;
  u64 td_phys;
  u8 *buf;
  u64 buf_phys;

  /* Volatile: written from IRQ context. */
  volatile u32 reports;
  volatile u32 errors;
  volatile u16 last_len;
};

struct uhci_hc {
  u16 io_base;
  u8  irq;

  u64 frame_list_phys;
  struct uhci_frame *frame_list;

  u64 arena_phys;
  u8 *arena;

  u8 next_addr; /* 0 is reserved for un-enumerated devices */

  struct uhci_int_ep int_ep;
};

static void uhci_write16(u16 base, u16 offset, u16 val) {
  outw(base + offset, val);
}

static u16 uhci_read16(u16 base, u16 offset) { return inw(base + offset); }

/* Single volatile u32 access , see UHCI_TD_STS_* in uhci.h. */
SINLINE u32 td_status_read(const struct uhci_td *td) {
  return *(const volatile u32 *)&td->status;
}

SINLINE void td_status_write(struct uhci_td *td, u32 val) {
  *(volatile u32 *)&td->status = val;
}

/* element_ptr is the CPU/controller hand-off point: volatile, or the compiler
 * caches our write and never sees the controller's DMA update. */
SINLINE u32 qh_element_read(const struct uhci_qh *qh) {
  return *(const volatile u32 *)(uptr)&qh->element_ptr;
}

SINLINE void qh_element_write(struct uhci_qh *qh, u32 val) {
    *(volatile u32 *)(uptr)&qh->element_ptr = val;
}

/* Poll `reg` until `mask` clears. Returns 0, or -1 on timeout.
 *
 * PIT-timed rather than counting io_wait()s: an io_wait's real cost varies by
 * host, so a loop count is not a time bound. QEMU clears these instantly, real
 * controllers take a documented number of milliseconds. */
static int uhci_wait_clear(u16 io_base, u16 reg, u16 mask, u32 budget_ms) {
  for (u32 elapsed = 0; elapsed < budget_ms * 10; elapsed++) {
    if (!(uhci_read16(io_base, reg) & mask))
      return 0;
    pit_delay_us(100);
  }
  return -1;
}

// #region CONTROL TRANSFERS

static struct uhci_td *uhci_td_at(struct uhci_hc *hc, int index) {
  return (struct uhci_td *)(hc->arena + UHCI_ARENA_TD +
                            (usize)index * sizeof(struct uhci_td));
}

static u64 uhci_td_phys(struct uhci_hc *hc, int index) {
  return hc->arena_phys + UHCI_ARENA_TD +
         (u64)index * sizeof(struct uhci_td);
}

/* Fill one TD. `len` is a real byte count; 0 is legal and UHCI_TD_MAXLEN
 * encodes it as the spec's 0x7FF null packet. */
static void uhci_td_init(struct uhci_td *td, u64 next_phys, int last,
                         int low_speed, u8 pid, u8 addr, u8 ep, int toggle,
                         int len, u64 buf_phys) {
  /* DEPTH runs the whole chain within one frame; without it the controller
   * takes one TD per frame and a control transfer costs milliseconds. */
  td->link.link_ptr =
      last ? UHCI_PTR_TERM : ((u32)next_phys | UHCI_PTR_DEPTH);

  u32 sts = UHCI_TD_STS_ACTIVE | (3u << UHCI_TD_STS_CERR_SHIFT);
  if (low_speed)
    sts |= UHCI_TD_STS_LS;
  td_status_write(td, sts);

  td->token = UHCI_TD_PID(pid) | UHCI_TD_DEVADDR(addr) |
              UHCI_TD_ENDPOINT(ep) | UHCI_TD_TOGGLE(toggle) |
              UHCI_TD_MAXLEN(len);
  td->buffer_ptr = (u32)buf_phys;
}

/* Run one control transfer on endpoint 0 and wait for it to retire. Returns
 * data-stage bytes moved, or -1.
 *
 * Polled, not interrupt-driven: this runs before the boot-time sti, so an
 * interrupt could not be delivered anyway. */
static int uhci_control(struct uhci_hc *hc, u8 addr, int low_speed,
                        u8 maxpacket,
                        const struct usb_setup_packet *setup, void *data,
                        u16 len, int dir_in) {
  if (len > UHCI_ARENA_DATA_MAX)
    return -1;
  if (maxpacket == 0)
    return -1;

  /* setup TD + one TD per data packet + status TD */
  int data_tds = (len + maxpacket - 1) / maxpacket;
  int n_tds = data_tds + 2;
  if (n_tds > UHCI_ARENA_NTD)
    return -1;

  u8 *setup_buf = hc->arena + UHCI_ARENA_SETUP;
  u8 *data_buf = hc->arena + UHCI_ARENA_DATA;
  u64 setup_phys = hc->arena_phys + UHCI_ARENA_SETUP;
  u64 data_phys = hc->arena_phys + UHCI_ARENA_DATA;

  memcpy(setup_buf, setup, sizeof(*setup));
  if (len && !dir_in)
    memcpy(data_buf, data, len);
  else
    memset(data_buf, 0, len);

  int i = 0;

  /* SETUP , always DATA0. */
  uhci_td_init(uhci_td_at(hc, i), uhci_td_phys(hc, i + 1), 0, low_speed,
               USB_PID_SETUP, addr, 0, 0, sizeof(*setup), setup_phys);
  i++;

  /* DATA , starts DATA1, alternates per packet. */
  int toggle = 1;
  u16 remaining = len;
  u32 offset = 0;
  while (remaining) {
    u16 chunk = remaining > maxpacket ? maxpacket : remaining;
    uhci_td_init(uhci_td_at(hc, i), uhci_td_phys(hc, i + 1), 0, low_speed,
                 dir_in ? USB_PID_IN : USB_PID_OUT, addr, 0, toggle, chunk,
                 data_phys + offset);
    i++;
    toggle ^= 1;
    offset += chunk;
    remaining -= chunk;
  }

  /* STATUS , opposite direction to the data stage, DATA1, zero length. With
   * no data stage the status is read IN. */
  uhci_td_init(uhci_td_at(hc, i), 0, 1, low_speed,
               dir_in ? USB_PID_OUT : USB_PID_IN, addr, 0, 1, 0, 0);
  int last_td = i;

  /* The QH already sits in every frame list entry, so writing element_ptr is
   * what starts the work. The controller walks it back to TERM as it drains. */
  struct uhci_qh *qh = (struct uhci_qh *)(hc->arena + UHCI_ARENA_QH);
  qh->link_ptr = UHCI_PTR_TERM;
  qh_element_write(qh, (u32)uhci_td_phys(hc, 0));

  int failed = 0;
  int done = 0;
  for (u32 elapsed = 0; elapsed < UHCI_XFER_TIMEOUT_MS * 10; elapsed++) {
    /* Scan the whole chain, not just the tail: an erroring TD halts the queue
     * and leaves everything behind it Active forever, which would otherwise
     * burn the full timeout.
     *
     * Errors only. A leading TD going inactive is normal , the chain retires
     * in order, so SETUP clears Active while DATA and STATUS are still
     * pending. Treating that as an early halt fails every transfer. */
    for (int t = 0; t <= last_td; t++) {
      if (td_status_read(uhci_td_at(hc, t)) & UHCI_TD_STS_ERRMASK) {
        failed = 1;
        break;
      }
    }
    if (failed)
      break;
    if (!(td_status_read(uhci_td_at(hc, last_td)) & UHCI_TD_STS_ACTIVE)) {
      done = 1;
      break;
    }
    pit_delay_us(100);
  }

  /* Snapshot before unhooking. Whether element_ptr moved off TD 0 separates
   * "device misbehaved" from "controller never looked at the queue". */
  u32 element_after = qh_element_read(qh);
  u16 usbsts = uhci_read16(hc->io_base, UHCI_USBSTS);
  u16 frnum = uhci_read16(hc->io_base, UHCI_FRNUM);

  /* Unhook either way: on timeout the controller still owns these TDs and the
   * next transfer reuses the pool. */
  qh_element_write(qh, UHCI_PTR_TERM);

  if (!done || failed) {
    log_write(failed ? "UHCI: transfer failed" : "UHCI: transfer timed out",
              KERNEL, LOG_ERROR);
    log_write_hex("UHCI:   USBSTS", usbsts, KERNEL, LOG_ERROR);
    log_write_hex("UHCI:   USBCMD", uhci_read16(hc->io_base, UHCI_USBCMD),
                  KERNEL, LOG_ERROR);
    log_write_hex("UHCI:   FRNUM", frnum, KERNEL, LOG_ERROR);
    log_write_hex("UHCI:   QH element after", element_after, KERNEL, LOG_ERROR);
    log_write_hex("UHCI:   QH element start", (u32)uhci_td_phys(hc, 0), KERNEL,
                  LOG_ERROR);
    for (int t = 0; t <= last_td; t++) {
      log_write_int("UHCI:   TD", t, KERNEL, LOG_ERROR);
      log_write_hex("UHCI:     status", td_status_read(uhci_td_at(hc, t)),
                    KERNEL, LOG_ERROR);
      log_write_hex("UHCI:     token", uhci_td_at(hc, t)->token, KERNEL,
                    LOG_ERROR);
    }
    return -1;
  }

  /* Sum the data stage: a device may answer with less than we asked for. */
  int transferred = 0;
  for (int t = 1; t < last_td; t++) {
    u32 sts = td_status_read(uhci_td_at(hc, t));
    transferred += (int)UHCI_TD_ACTLEN(sts);
  }

  if (len && dir_in)
    memcpy(data, data_buf, (usize)transferred < len ? (usize)transferred
                                                     : len);

  return transferred;
}

static int uhci_get_descriptor(struct uhci_hc *hc, u8 addr, int low_speed,
                               u8 maxpacket, u8 type, u8 index, void *out,
                               u16 len) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_IN_STD_DEVICE,
      .bRequest = USB_REQ_GET_DESCRIPTOR,
      .wValue = (u16)((type << 8) | index),
      .wIndex = 0,
      .wLength = len,
  };
  return uhci_control(hc, addr, low_speed, maxpacket, &setup, out, len, 1);
}

static int uhci_set_address(struct uhci_hc *hc, int low_speed, u8 maxpacket,
                            u8 new_addr) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_OUT_STD_DEVICE,
      .bRequest = USB_REQ_SET_ADDRESS,
      .wValue = new_addr,
      .wIndex = 0,
      .wLength = 0,
  };
  /* Sent to address 0: the device adopts new_addr only once status completes. */
  return uhci_control(hc, 0, low_speed, maxpacket, &setup, 0, 0, 0);
}

static int uhci_set_configuration(struct uhci_hc *hc, u8 addr, int low_speed,
                                  u8 maxpacket, u8 config) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_OUT_STD_DEVICE,
      .bRequest = USB_REQ_SET_CONFIGURATION,
      .wValue = config,
      .wIndex = 0,
      .wLength = 0,
  };
  return uhci_control(hc, addr, low_speed, maxpacket, &setup, 0, 0, 0);
}

static int uhci_hid_set_idle(struct uhci_hc *hc, u8 addr, int low_speed,
                             u8 maxpacket, u8 interface) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_OUT_CLASS_INTERFACE,
      .bRequest = USB_REQ_SET_IDLE,
      .wValue = 0,
      .wIndex = interface,
      .wLength = 0,
  };
  return uhci_control(hc, addr, low_speed, maxpacket, &setup, 0, 0, 0);
}

static int uhci_hid_set_boot_protocol(struct uhci_hc *hc, u8 addr,
                                      int low_speed, u8 maxpacket,
                                      u8 interface) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_OUT_CLASS_INTERFACE,
      .bRequest = USB_REQ_SET_PROTOCOL,
      .wValue = 0, /* boot protocol */
      .wIndex = interface,
      .wLength = 0,
  };
  return uhci_control(hc, addr, low_speed, maxpacket, &setup, 0, 0, 0);
}

// #endregion CONTROL TRANSFERS

// #region PERIODIC (INTERRUPT) ENDPOINTS

/* (Re)build the periodic TD and hand it to the controller.
 *
 * IOC is set here and nowhere else: control transfers are polled and want no
 * interrupt, but nothing watches a periodic endpoint , the completion
 * interrupt is the only signal that a report arrived. */
static void uhci_int_arm(struct uhci_int_ep *ep) {
  struct uhci_td *td = ep->td;

  td->link.link_ptr = UHCI_PTR_TERM;

  u32 sts = UHCI_TD_STS_ACTIVE | UHCI_TD_STS_IOC |
            (3u << UHCI_TD_STS_CERR_SHIFT);
  if (ep->low_speed)
    sts |= UHCI_TD_STS_LS;
  td_status_write(td, sts);

  td->token = UHCI_TD_PID(USB_PID_IN) | UHCI_TD_DEVADDR(ep->addr) |
              UHCI_TD_ENDPOINT(ep->ep) | UHCI_TD_TOGGLE(ep->toggle) |
              UHCI_TD_MAXLEN(ep->maxlen);
  td->buffer_ptr = (u32)ep->buf_phys;

  /* Last: this is what makes the queue live, so the TD must be complete. */
  qh_element_write(ep->qh, (u32)ep->td_phys);
}

/* Frame list points at the periodic queue every `interval` frames, control
 * queue otherwise. The interrupt QH links onward to the control QH, so control
 * stays reachable from every frame; the interval only sets the poll rate. */
static void uhci_build_schedule(struct uhci_hc *hc) {
  u64 ctrl_qh_phys = hc->arena_phys + UHCI_ARENA_QH;
  u32 ctrl_link = (u32)ctrl_qh_phys | UHCI_PTR_QH;

  if (!hc->int_ep.active) {
    for (int i = 0; i < 1024; i++)
      hc->frame_list[i].link_ptr = ctrl_link;
    return;
  }

  u32 int_link = (u32)hc->int_ep.qh_phys | UHCI_PTR_QH;
  hc->int_ep.qh->link_ptr = ctrl_link;

  u8 interval = hc->int_ep.interval ? hc->int_ep.interval : 1;
  for (int i = 0; i < 1024; i++)
    hc->frame_list[i].link_ptr = (i % interval == 0) ? int_link : ctrl_link;
}

static int uhci_int_ep_start(struct uhci_hc *hc, u8 addr, int low_speed, u8 ep,
                             u16 maxlen, u8 interval, u8 kind) {
  struct uhci_int_ep *iep = &hc->int_ep;

  if (iep->active) {
    log_write("UHCI: periodic endpoint already in use, skipping", KERNEL,
              LOG_WARN);
    return -1;
  }
  if (maxlen == 0 || maxlen > UHCI_ARENA_INT_MAX) {
    log_write_int("UHCI: periodic packet size unsupported", maxlen, KERNEL,
                  LOG_WARN);
    return -1;
  }

  iep->addr = addr;
  iep->ep = ep;
  iep->low_speed = low_speed;
  iep->maxlen = maxlen;
  iep->interval = interval;
  iep->kind = kind;
  iep->toggle = 0;
  iep->reports = 0;
  iep->errors = 0;
  iep->last_len = 0;

  iep->qh = (struct uhci_qh *)(hc->arena + UHCI_ARENA_INT_QH);
  iep->qh_phys = hc->arena_phys + UHCI_ARENA_INT_QH;
  iep->td = (struct uhci_td *)(hc->arena + UHCI_ARENA_INT_TD);
  iep->td_phys = hc->arena_phys + UHCI_ARENA_INT_TD;
  iep->buf = hc->arena + UHCI_ARENA_INT_BUF;
  iep->buf_phys = hc->arena_phys + UHCI_ARENA_INT_BUF;

  memset(iep->buf, 0, UHCI_ARENA_INT_MAX);
  qh_element_write(iep->qh, UHCI_PTR_TERM);

  iep->active = 1;

  /* Schedule before arming, so the first completion has somewhere to land. */
  uhci_build_schedule(hc);
  uhci_int_arm(iep);
  return 0;
}

/* Service a completed periodic TD. IRQ context. */
static void uhci_int_complete(struct uhci_int_ep *ep) {
  if (!ep->active)
    return;

  u32 sts = td_status_read(ep->td);
  if (sts & UHCI_TD_STS_ACTIVE)
    return; /* device has nothing to say yet */

  if (sts & UHCI_TD_STS_ERRMASK) {
    ep->errors++;
    /* Re-armed below regardless: a transient CRC or timeout is normal on real
     * hardware and must not stop polling permanently. Toggle stays put , the
     * failed transfer never advanced the device's sequence, and flipping it
     * would desync the endpoint for good. */
    if (ep->errors <= UHCI_INT_LOG_LIMIT)
      log_write_hex("UHCI: periodic transfer error, status", sts, KERNEL,
                    LOG_WARN);
  } else {
    ep->reports++;
    ep->last_len = (u16)UHCI_TD_ACTLEN(sts);
    ep->toggle ^= 1;

    if (ep->kind == UHCI_INT_KIND_HID_BOOT_MOUSE)
      mouse_hid_report(ep->buf, ep->last_len);
    else if (ep->kind == UHCI_INT_KIND_HID_TABLET)
      mouse_hid_tablet_report(ep->buf, ep->last_len);

    /* Bounded: IRQ context writing to a polled UART. Enough to prove reports
     * arrive, then silence , uncapped, a moving mouse stalls the handler. */
    if (ep->reports <= UHCI_INT_LOG_LIMIT) {
      log_write_int("UHCI: report bytes", ep->last_len, KERNEL, LOG_INFO);
      u32 head = 0;
      for (int i = 0; i < 4 && i < ep->last_len; i++)
        head |= (u32)ep->buf[i] << (i * 8);
      log_write_hex("UHCI:   data[0..3]", head, KERNEL, LOG_INFO);
    }
  }

  uhci_int_arm(ep);
}

// #endregion PERIODIC (INTERRUPT) ENDPOINTS

// #region PORTS + ENUMERATION

/* Read-modify-write PORTSC. An absolute write is wrong twice over: it drops
 * whatever else the port reports, and a 1 in CSC or PEC acknowledges a change
 * nobody handled. Masking UHCI_PORTSC_WC_MASK moves only the requested bits. */
static void uhci_portsc_set(struct uhci_hc *hc, u16 reg, u16 bits) {
  u16 val = uhci_read16(hc->io_base, reg);
  val = (u16)((val | bits) & ~UHCI_PORTSC_WC_MASK);
  uhci_write16(hc->io_base, reg, val);
}

static void uhci_portsc_clear(struct uhci_hc *hc, u16 reg, u16 bits) {
  u16 val = uhci_read16(hc->io_base, reg);
  val = (u16)(val & ~bits & ~UHCI_PORTSC_WC_MASK);
  uhci_write16(hc->io_base, reg, val);
}

/* Reset and enable one root port. Returns 0 if it came up enabled. */
static int uhci_port_reset(struct uhci_hc *hc, u16 reg) {
  /* Spec minimum is 10 ms of reset; 50 is what real drivers use. */
  uhci_portsc_set(hc, reg, UHCI_PORTSC_PR);
  pit_delay_ms(50);
  uhci_portsc_clear(hc, reg, UHCI_PORTSC_PR);
  pit_delay_us(10); /* recovery before the port accepts an enable */

  uhci_portsc_set(hc, reg, UHCI_PORTSC_PE);

  int enabled = 0;
  for (int attempt = 0; attempt < 100; attempt++) {
    u16 status = uhci_read16(hc->io_base, reg);

    if (!(status & UHCI_PORTSC_CCS))
      return -1; /* device went away mid-reset */
    if (status & UHCI_PORTSC_PE) {
      enabled = 1;
      break;
    }

    /* Re-assert: the controller drops PE if the port glitches leaving reset. */
    uhci_portsc_set(hc, reg, UHCI_PORTSC_PE);
    pit_delay_ms(1);
  }
  if (!enabled)
    return -1;

  /* Acknowledge the changes this sequence generated, keeping PE set. */
  u16 status = uhci_read16(hc->io_base, reg);
  uhci_write16(hc->io_base, reg, (u16)(status | UHCI_PORTSC_WC_MASK));
  return 0;
}

/* Static, not stack: enumeration runs on the init task's kernel stack and a
 * device may claim several hundred bytes. Boot path is single-threaded. */
static u8 uhci_config_buf[UHCI_CONFIG_BUF_MAX];

/* Read configuration `config_index` into uhci_config_buf. Returns its length,
 * or -1. Two requests by necessity: wTotalLength is only known after the
 * 9-byte header, and over-asking is not allowed to work. */
static int uhci_read_config(struct uhci_hc *hc, u8 addr, int low_speed,
                            u8 maxpacket, u8 config_index) {
  struct usb_config_descriptor head;

  memset(&head, 0, sizeof(head));
  if (uhci_get_descriptor(hc, addr, low_speed, maxpacket,
                          USB_DESC_CONFIGURATION, config_index, &head,
                          sizeof(head)) < (int)sizeof(head)) {
    log_write("UHCI: config header read failed", KERNEL, LOG_ERROR);
    return -1;
  }

  u16 total = head.wTotalLength;
  if (total < sizeof(head)) {
    log_write_int("UHCI: config wTotalLength nonsensical", total, KERNEL,
                  LOG_ERROR);
    return -1;
  }
  if (total > UHCI_CONFIG_BUF_MAX) {
    log_write_int("UHCI: config block too large, truncating", total, KERNEL,
                  LOG_WARN);
    total = UHCI_CONFIG_BUF_MAX;
  }

  memset(uhci_config_buf, 0, sizeof(uhci_config_buf));
  int got = uhci_get_descriptor(hc, addr, low_speed, maxpacket,
                                USB_DESC_CONFIGURATION, config_index,
                                uhci_config_buf, total);
  if (got < (int)sizeof(head)) {
    log_write_int("UHCI: config block read failed, got", got, KERNEL,
                  LOG_ERROR);
    return -1;
  }
  return got;
}

/* Find the first HID pointer interrupt IN endpoint, logging interfaces and
 * endpoints on the way past. Returns 1 and fills *out if one exists.
 *
 * The block is a chain of variable-length descriptors, each led by {bLength,
 * bDescriptorType}. Stepping by bLength rather than assuming fixed sizes is
 * what skips class-specific descriptors we do not parse , a HID report
 * descriptor sits between the interface and its endpoints. */
struct uhci_hid_pointer {
  u8 interface_number;
  u8 kind;
  struct usb_endpoint_descriptor endpoint;
};

static int uhci_find_hid_pointer(const u8 *buf, int len,
                                 struct uhci_hid_pointer *out) {
  int found = 0;
  int current_hid_interface = 0;
  u8 current_kind = UHCI_INT_KIND_NONE;
  u8 current_interface = 0;
  int offset = 0;

  while (offset + 2 <= len) {
    u8 blen = buf[offset];
    u8 btype = buf[offset + 1];

    /* blen 0 would spin forever; overrunning means the device lied about
     * wTotalLength. Stop either way. */
    if (blen < 2 || offset + blen > len)
      break;

    if (btype == USB_DESC_INTERFACE &&
        blen >= sizeof(struct usb_interface_descriptor)) {
      const struct usb_interface_descriptor *ifd =
          (const struct usb_interface_descriptor *)(buf + offset);
      log_write_int("UHCI:   interface", ifd->bInterfaceNumber, KERNEL,
                    LOG_INFO);
      log_write_hex("UHCI:     class", ifd->bInterfaceClass, KERNEL, LOG_INFO);
      log_write_hex("UHCI:     subclass", ifd->bInterfaceSubClass, KERNEL,
                    LOG_INFO);
      log_write_hex("UHCI:     protocol", ifd->bInterfaceProtocol, KERNEL,
                    LOG_INFO);
      log_write_int("UHCI:     endpoints", ifd->bNumEndpoints, KERNEL,
                    LOG_INFO);
      current_interface = ifd->bInterfaceNumber;
      current_hid_interface = ifd->bInterfaceClass == USB_CLASS_HID;
      current_kind = UHCI_INT_KIND_NONE;
      if (current_hid_interface &&
          ifd->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
          ifd->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE) {
        current_kind = UHCI_INT_KIND_HID_BOOT_MOUSE;
      }
    } else if (btype == USB_DESC_HID && blen >= 9) {
      u16 report_len = (u16)buf[offset + 7] | ((u16)buf[offset + 8] << 8);
      log_write_int("UHCI:   HID report length", report_len, KERNEL,
                    LOG_INFO);

      /* QEMU usb-tablet is a HID pointer with protocol 0 and a 74-byte report
       * descriptor. We do not have a full HID report parser yet, so keep this
       * match deliberately narrow instead of treating every vendor HID as a
       * mouse. */
      if (current_hid_interface && current_kind == UHCI_INT_KIND_NONE &&
          report_len == 74) {
        current_kind = UHCI_INT_KIND_HID_TABLET;
      }
    } else if (btype == USB_DESC_ENDPOINT &&
               blen >= sizeof(struct usb_endpoint_descriptor)) {
      const struct usb_endpoint_descriptor *epd =
          (const struct usb_endpoint_descriptor *)(buf + offset);
      log_write_hex("UHCI:   endpoint addr", epd->bEndpointAddress, KERNEL,
                    LOG_INFO);
      log_write_hex("UHCI:     attributes", epd->bmAttributes, KERNEL,
                    LOG_INFO);
      log_write_int("UHCI:     max packet", epd->wMaxPacketSize, KERNEL,
                    LOG_INFO);
      log_write_int("UHCI:     interval", epd->bInterval, KERNEL, LOG_INFO);

      u16 max_packet = epd->wMaxPacketSize & 0x07FF;
      int packet_fits_kind =
          current_kind != UHCI_INT_KIND_HID_TABLET || max_packet >= 5;
      if (!found && current_kind != UHCI_INT_KIND_NONE && packet_fits_kind &&
          (epd->bmAttributes & USB_EP_XFER_MASK) == USB_EP_XFER_INTERRUPT &&
          (epd->bEndpointAddress & USB_EP_ADDR_DIR_IN)) {
        out->interface_number = current_interface;
        out->kind = current_kind;
        out->endpoint = *epd;
        out->endpoint.wMaxPacketSize = max_packet;
        found = 1;
      }
    }

    offset += blen;
  }

  return found;
}

static void uhci_enumerate_port(struct uhci_hc *hc, int port, u16 reg,
                                int low_speed) {
  struct usb_descriptor desc;

  if (uhci_port_reset(hc, reg) != 0) {
    log_write_int("UHCI: port failed to enable, port", port, KERNEL, LOG_WARN);
    return;
  }
  log_write_int("UHCI: port enabled, port", port, KERNEL, LOG_INFO);

  /* Chicken and egg: the descriptor names ep0's packet size, but reading it
   * needs one. Every device accepts 8 and bMaxPacketSize0 is byte 7, so an
   * 8-byte read always reaches it. */
  memset(&desc, 0, sizeof(desc));
  if (uhci_get_descriptor(hc, 0, low_speed, UHCI_EP0_DEFAULT_MAXPACKET,
                          USB_DESC_DEVICE, 0, &desc, 8) < 8) {
    log_write_int("UHCI: initial GET_DESCRIPTOR failed, port", port, KERNEL,
                  LOG_ERROR);
    return;
  }

  u8 maxpacket = desc.bMaxPacketSize0;
  if (maxpacket == 0)
    maxpacket = UHCI_EP0_DEFAULT_MAXPACKET;
  log_write_int("UHCI: ep0 max packet", maxpacket, KERNEL, LOG_INFO);

  /* Devices may expect a reset between the probe read and SET_ADDRESS, and
   * many real ones require it. */
  if (uhci_port_reset(hc, reg) != 0) {
    log_write_int("UHCI: port dropped after probe, port", port, KERNEL,
                  LOG_ERROR);
    return;
  }

  u8 addr = hc->next_addr;
  if (uhci_set_address(hc, low_speed, maxpacket, addr) < 0) {
    log_write_int("UHCI: SET_ADDRESS failed, port", port, KERNEL, LOG_ERROR);
    return;
  }
  hc->next_addr++;

  pit_delay_ms(2); /* SetAddress recovery: 2 ms before it must answer */
  log_write_int("UHCI: device addressed", addr, KERNEL, LOG_INFO);

  memset(&desc, 0, sizeof(desc));
  if (uhci_get_descriptor(hc, addr, low_speed, maxpacket, USB_DESC_DEVICE, 0,
                          &desc, sizeof(desc)) < (int)sizeof(desc)) {
    log_write_int("UHCI: full GET_DESCRIPTOR failed, addr", addr, KERNEL,
                  LOG_ERROR);
    return;
  }

  log_write_hex("UHCI:   idVendor", desc.idVendor, KERNEL, LOG_INFO);
  log_write_hex("UHCI:   idProduct", desc.idProduct, KERNEL, LOG_INFO);
  log_write_hex("UHCI:   bcdUSB", desc.bcdUSB, KERNEL, LOG_INFO);
  log_write_hex("UHCI:   bDeviceClass", desc.bDeviceClass, KERNEL, LOG_INFO);
  log_write_int("UHCI:   configurations", desc.bNumConfigurations, KERNEL,
                LOG_INFO);

  if (desc.bNumConfigurations == 0) {
    log_write("UHCI: device offers no configuration", KERNEL, LOG_WARN);
    return;
  }

  int cfg_len = uhci_read_config(hc, addr, low_speed, maxpacket, 0);
  if (cfg_len < 0)
    return;

  const struct usb_config_descriptor *cfg =
      (const struct usb_config_descriptor *)uhci_config_buf;
  log_write_int("UHCI:   config value", cfg->bConfigurationValue, KERNEL,
                LOG_INFO);
  log_write_int("UHCI:   interfaces", cfg->bNumInterfaces, KERNEL, LOG_INFO);

  struct uhci_hid_pointer hid_pointer;
  memset(&hid_pointer, 0, sizeof(hid_pointer));
  int have_hid_pointer = uhci_find_hid_pointer(uhci_config_buf, cfg_len,
                                               &hid_pointer);

  /* Until this lands the device answers only standard requests on ep0 , its
   * other endpoints do not exist, so configure before scheduling anything. */
  if (uhci_set_configuration(hc, addr, low_speed, maxpacket,
                             cfg->bConfigurationValue) < 0) {
    log_write_int("UHCI: SET_CONFIGURATION failed, addr", addr, KERNEL,
                  LOG_ERROR);
    return;
  }
  log_write_int("UHCI: device configured", cfg->bConfigurationValue, KERNEL,
                LOG_INFO);

  if (!have_hid_pointer) {
    log_write("UHCI: no supported HID pointer endpoint, nothing to poll",
              KERNEL, LOG_INFO);
    return;
  }

  if (uhci_hid_set_idle(hc, addr, low_speed, maxpacket,
                        hid_pointer.interface_number) < 0) {
    log_write("UHCI: HID SET_IDLE failed, continuing", KERNEL, LOG_WARN);
  }
  if (hid_pointer.kind == UHCI_INT_KIND_HID_BOOT_MOUSE) {
    if (uhci_hid_set_boot_protocol(hc, addr, low_speed, maxpacket,
                                   hid_pointer.interface_number) < 0) {
      log_write("UHCI: HID SET_PROTOCOL boot failed, continuing", KERNEL,
                LOG_WARN);
    }
  }

  struct usb_endpoint_descriptor *int_ep = &hid_pointer.endpoint;
  u8 ep_num = int_ep->bEndpointAddress & USB_EP_ADDR_NUM_MASK;
  if (uhci_int_ep_start(hc, addr, low_speed, ep_num, int_ep->wMaxPacketSize,
                        int_ep->bInterval, hid_pointer.kind) == 0) {
    log_write_int("UHCI: polling HID pointer endpoint", ep_num, KERNEL,
                  LOG_INFO);
  }
}

static void uhci_scan_ports(struct uhci_hc *hc) {
  for (int port = 0; port < UHCI_NUM_PORTS; port++) {
    u16 reg = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;
    u16 status = uhci_read16(hc->io_base, reg);

    log_write_int("UHCI: scanning port", port, KERNEL, LOG_INFO);
    log_write_hex("UHCI:   PORTSC", status, KERNEL, LOG_INFO);

    if (!(status & UHCI_PORTSC_CCS)) {
      log_write_int("UHCI: no device on port", port, KERNEL, LOG_INFO);
      continue;
    }

    int low_speed = (status & UHCI_PORTSC_LSDA) != 0;
    log_write(low_speed ? "UHCI: device attached (low speed)"
                        : "UHCI: device attached (full speed)",
              KERNEL, LOG_INFO);

    uhci_enumerate_port(hc, port, reg, low_speed);
  }
}

// #endregion PORTS + ENUMERATION

/* File scope, not uhci_init's stack: the controller DMAs from the arena and
 * frame list for as long as it runs, and the IRQ handler needs to find them.
 * Sized for several , ICH9 fronts one EHCI with three companion UHCIs. */
#define UHCI_MAX_CONTROLLERS 4
static struct uhci_hc uhci_controllers[UHCI_MAX_CONTROLLERS];
static int uhci_controller_count;

/* PCI interrupt lines are shared, so this also fires for other devices. Ask
 * every controller, and leave one with a clear USBSTS entirely alone , acking
 * a status it never raised would drop someone else's event. The IDT dispatcher
 * sends the EOI. */
static void uhci_irq_handler(void) {
  for (int i = 0; i < uhci_controller_count; i++) {
    struct uhci_hc *hc = &uhci_controllers[i];
    if (!hc->io_base)
      continue;

    u16 sts = uhci_read16(hc->io_base, UHCI_USBSTS);
    if (!(sts & (UHCI_USBSTS_INTERRUPT | UHCI_USBSTS_USBERR)))
      continue;

    uhci_write16(hc->io_base, UHCI_USBSTS, sts); /* W1C, observed bits only */
    uhci_int_complete(&hc->int_ep);
  }
}

int uhci_init(struct pci_device *dev) {
  if (uhci_controller_count >= UHCI_MAX_CONTROLLERS) {
    log_write("UHCI: controller table full, skipping", KERNEL, LOG_WARN);
    return -1;
  }

  struct uhci_hc *hc = &uhci_controllers[uhci_controller_count];
  memset(hc, 0, sizeof(*hc));
  hc->next_addr = 1;

  log_write("UHCI: Initializing...", KERNEL, LOG_INFO);
  /* usb_init() already did pci_enable() before dispatching on prog_if. */

  /* UHCI is an I/O-BAR device; BAR4 is fixed by the spec. */
  if (!dev->bar[4].valid || !dev->bar[4].is_io) {
    log_write("UHCI: BAR4 is not a valid I/O BAR", KERNEL, LOG_ERROR);
    return -1;
  }
  hc->io_base = (u16)dev->bar[4].base;
  hc->irq = dev->int_line;
  log_write_hex("UHCI: I/O Base", hc->io_base, KERNEL, LOG_INFO);
  log_write_int("UHCI: IRQ line", hc->irq, KERNEL, LOG_INFO);

  /* Frame list: 1024 x 4 B, exactly one page, and 4KB aligned as required. */
  hc->frame_list_phys = pmm_alloc_frame_below(UHCI_DMA_LIMIT);
  if (!hc->frame_list_phys)
    return -1;
  hc->frame_list = (struct uhci_frame *)phys_to_virt(hc->frame_list_phys);

  hc->arena_phys = pmm_alloc_frame_below(UHCI_DMA_LIMIT);
  if (!hc->arena_phys) {
    pmm_free_frame(hc->frame_list_phys);
    return -1;
  }
  hc->arena = (u8 *)phys_to_virt(hc->arena_phys);
  memset(hc->arena, 0, FRAME_SIZE);

  /* Idle control queue: element_ptr TERM, so the controller walks
   * frame -> QH -> nothing. link_ptr must terminate too , a QH pointing at
   * itself loops the controller for the rest of the frame. */
  struct uhci_qh *qh = (struct uhci_qh *)(hc->arena + UHCI_ARENA_QH);
  qh->link_ptr = UHCI_PTR_TERM;
  qh_element_write(qh, UHCI_PTR_TERM);

  /* Rebuilt later if enumeration finds an endpoint to poll. */
  uhci_build_schedule(hc);

  uhci_write16(hc->io_base, UHCI_USBCMD, UHCI_USBCMD_GRESET);
  pit_delay_ms(50);
  uhci_write16(hc->io_base, UHCI_USBCMD, 0);
  pit_delay_ms(10);

  uhci_write16(hc->io_base, UHCI_USBCMD, UHCI_USBCMD_HCRESET);

  /* Self-clears on completion, spec-bounded well under 10 ms. */
  if (uhci_wait_clear(hc->io_base, UHCI_USBCMD, UHCI_USBCMD_HCRESET, 100) != 0) {
    log_write("UHCI: HCRESET did not clear", KERNEL, LOG_ERROR);
    goto fail_free;
  }

  /* Registers reset with the controller, so program them after HCRESET. */
  outl(hc->io_base + UHCI_FRBASEADD, (u32)hc->frame_list_phys);
  outb(hc->io_base + UHCI_SOFMOD, 64);
  uhci_write16(hc->io_base, UHCI_FRNUM, 0);
  uhci_write16(hc->io_base, UHCI_USBSTS, 0xFFFF);

  /* Masked through bring-up and enumeration: those transfers are polled and
   * run before the boot-time sti, so arming IOC now would only strand an
   * assert on a shared line. Enabled at the end of init, if anything needs it. */
  uhci_write16(hc->io_base, UHCI_USBINTR, 0);

  uhci_write16(hc->io_base, UHCI_USBCMD,
               UHCI_USBCMD_CF | UHCI_USBCMD_RS | UHCI_USBCMD_MAXP);

  if (uhci_wait_clear(hc->io_base, UHCI_USBSTS, UHCI_USBSTS_HCHALTED, 100) !=
      0) {
    log_write("UHCI: controller stayed halted after Run/Stop", KERNEL,
              LOG_ERROR);
    goto fail_stop;
  }

  log_write("UHCI: Controller started", KERNEL, LOG_INFO);

  /* Commit the slot only once it is running: a failed bring-up must not leave
   * a half-initialised entry for the IRQ handler to walk into. */
  uhci_controller_count++;

  uhci_scan_ports(hc);

  /* Polled transfers are done, so hand the periodic endpoint to interrupts.
   * Handler and PIC unmask first, USBINTR last, so the first completion has
   * somewhere to go. Nothing fires until main() runs sti. */
  if (hc->int_ep.active) {
    irq_install(hc->irq, uhci_irq_handler);
    pic_clear_mask(hc->irq);
    uhci_write16(hc->io_base, UHCI_USBINTR,
                 UHCI_USBINTR_IOC_EN | UHCI_USBINTR_TIMEOUT_CRC_EN);
    log_write_int("UHCI: interrupts enabled on IRQ", hc->irq, KERNEL, LOG_INFO);
  }

  return 0;

fail_stop:
  /* Stop before freeing: the controller must not be left DMAing into pages
   * about to go back to the PMM. */
  uhci_write16(hc->io_base, UHCI_USBCMD, 0);
  pit_delay_ms(1); /* let any in-flight frame drain */
fail_free:
  pmm_free_frame(hc->arena_phys);
  pmm_free_frame(hc->frame_list_phys);
  return -1;
}
