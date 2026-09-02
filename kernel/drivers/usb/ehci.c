/* EHCI USB 2.0 host controller.
 *
 * High-speed root devices are handled here. Full/low-speed devices are
 * routed to an OHCI/UHCI companion, as EHCI requires. Endpoint-zero control
 * requests use the asynchronous schedule; one HID pointer interrupt-IN
 * endpoint per controller can use the periodic schedule.
 */
#include <devices/pit.h>
#include <devices/usb.h>
#include <drivers/usb/ehci.h>
#include <drivers/usb/storage/usb_storage.h>
#include <input/mouse.h>
#include <interrupts/idt.h>
#include <interrupts/pic.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <pci/pci.h>
#include <utilities/log.h>
#include <utilities/string.h>

#define EHCI_DMA_LIMIT          0x100000000ULL
#define EHCI_MMIO_VIRT_BASE     0xFFFFE00500000000ULL
#define EHCI_MMIO_SLOT_SIZE     0x10000ULL
#define EHCI_MAX_CONTROLLERS    4
#define EHCI_CONFIG_BUF_MAX     256
#define EHCI_XFER_TIMEOUT_MS    500
#define EHCI_EP0_DEFAULT_MAXP   8
#define EHCI_INT_BUF_MAX        64
#define EHCI_INT_LOG_LIMIT      4

/* One physically contiguous 4 KiB DMA arena. All hardware link pointers are
 * 32-bit, and every QH/qTD address below meets its 32-byte alignment rule. */
#define EHCI_ARENA_ASYNC_HEAD   0x000
#define EHCI_ARENA_CONTROL_QH   0x040
#define EHCI_ARENA_QTD_POOL     0x080
#define EHCI_ARENA_SETUP        0x180
#define EHCI_ARENA_DATA         0x200
#define EHCI_ARENA_DATA_MAX     1024
#define EHCI_ARENA_PERIODIC_QH  0x600
#define EHCI_ARENA_PERIODIC_QTD 0x640
#define EHCI_ARENA_INT_BUF      0x680
#define EHCI_ARENA_BULK_BUF     0x800
#define EHCI_ARENA_BULK_MAX     2048

#define EHCI_INT_KIND_NONE           0
#define EHCI_INT_KIND_HID_BOOT_MOUSE 1
#define EHCI_INT_KIND_HID_TABLET     2

struct ehci_int_ep {
  int active;
  u8 addr;
  u8 ep;
  u16 maxlen;
  u8 interval;
  u8 kind;
  int toggle;
  struct ehci_qh *qh;
  u64 qh_phys;
  struct ehci_qtd *qtd;
  u64 qtd_phys;
  u8 *buf;
  u64 buf_phys;
  volatile u32 reports;
  volatile u32 errors;
  volatile u16 last_len;
};

struct ehci_hcd;
struct ehci_storage_device {
  struct ehci_hcd *hc;
  u8 address, port;
  u16 maxpacket;
  int disconnected;
};

struct ehci_hcd {
  struct pci_addr pci_addr;
  uintptr_t mmio;
  uintptr_t op;
  u64 mmio_page;
  u64 mmio_pages;
  u8 irq;
  u8 ports;
  u8 port_power_control;
  u8 next_addr;
  u64 periodic_phys;
  u32 *periodic;
  u64 arena_phys;
  u8 *arena;
  struct ehci_int_ep int_ep;
  struct spinlock async_lock;
  int async_failed;
  struct ehci_storage_device storage[15];
};

struct ehci_hid_pointer {
  u8 interface_number;
  u8 kind;
  struct usb_endpoint_descriptor endpoint;
};

static struct ehci_hcd ehci_controllers[EHCI_MAX_CONTROLLERS];
static int ehci_controller_count;
static u32 ehci_mmio_slot;
static u8 ehci_config_buf[EHCI_CONFIG_BUF_MAX];

SINLINE u8 ehci_read8(uintptr_t addr) { return read8(addr); }
SINLINE u16 ehci_read16(uintptr_t addr) { return read16(addr); }
SINLINE u32 ehci_read32(uintptr_t addr) { return read32(addr); }
SINLINE void ehci_write32(uintptr_t addr, u32 value) {
  write32(addr, value);
}
SINLINE u32 ehci_op_read(const struct ehci_hcd *hc, u32 reg) {
  return ehci_read32(hc->op + reg);
}
SINLINE void ehci_op_write(const struct ehci_hcd *hc, u32 reg, u32 value) {
  ehci_write32(hc->op + reg, value);
}
SINLINE u32 qtd_token_read(const struct ehci_qtd *qtd) {
  return *(const volatile u32 *)(const void *)&qtd->token;
}
SINLINE void qtd_token_write(struct ehci_qtd *qtd, u32 value) {
  *(volatile u32 *)(void *)&qtd->token = value;
}
SINLINE u32 qh_next_read(const struct ehci_qh *qh) {
  return *(const volatile u32 *)(const void *)&qh->overlay.next;
}
SINLINE void qh_next_write(struct ehci_qh *qh, u32 value) {
  *(volatile u32 *)(void *)&qh->overlay.next = value;
}
static void ehci_dma_barrier(void) {
  __asm__ volatile("mfence" ::: "memory");
}

static int ehci_wait_mask(uintptr_t base, u32 reg, u32 mask, u32 expected,
                          u32 budget_ms) {
  for (u64 i = 0; i < (u64)budget_ms * 10; i++) {
    if ((ehci_read32(base + reg) & mask) == expected)
      return 0;
    pit_delay_us(100);
  }
  return -1;
}
static int ehci_wait_clear(uintptr_t base, u32 reg, u32 mask, u32 budget_ms) {
  return ehci_wait_mask(base, reg, mask, 0, budget_ms);
}
static int ehci_wait_set(uintptr_t base, u32 reg, u32 mask, u32 budget_ms) {
  return ehci_wait_mask(base, reg, mask, mask, budget_ms);
}

static uintptr_t ehci_map_mmio(const struct pci_bar *bar, u64 *mapped_page,
                               u64 *mapped_pages) {
  if (!bar || !bar->valid || bar->is_io || bar->size == 0 ||
      ehci_mmio_slot >= EHCI_MAX_CONTROLLERS)
    return 0;
  u64 offset = bar->base & 0xFFFULL;
  u64 page_phys = bar->base & ~0xFFFULL;
  u64 pages = (bar->size + offset + 0xFFFULL) / 4096;
  if (pages == 0 || pages * 4096 > EHCI_MMIO_SLOT_SIZE)
    return 0;

  u64 va = EHCI_MMIO_VIRT_BASE +
           (u64)ehci_mmio_slot * EHCI_MMIO_SLOT_SIZE;
  u64 flags = VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT | VMM_NX;
  for (u64 i = 0; i < pages; i++) {
    if (vmm_map(va + i * 4096, page_phys + i * 4096, flags) != 0) {
      while (i > 0) {
        i--;
        vmm_unmap(va + i * 4096);
      }
      return 0;
    }
  }
  ehci_mmio_slot++;
  *mapped_page = va;
  *mapped_pages = pages;
  return (uintptr_t)(va + offset);
}

static void ehci_unmap_mmio(struct ehci_hcd *hc) {
  for (u64 i = 0; i < hc->mmio_pages; i++)
    vmm_unmap(hc->mmio_page + i * 4096);
  hc->mmio = 0;
  hc->op = 0;
}

/* EHCI extended capability ownership handoff (EHCI 1.0 section 2.1.7). */
static int ehci_take_ownership(struct ehci_hcd *hc, u32 hccparams) {
  u8 eecp = (u8)FIELD_GET(HCCPARAMS_REG_EECP, hccparams);
  for (int hops = 0; eecp >= 0x40 && hops < 48; hops++) {
    u32 legsup = pci_read32(hc->pci_addr, eecp);
    u8 next = (u8)FIELD_GET(USBLEGSUP_NEXT_MASK, legsup);
    if ((u8)legsup == USBLEGSUP_CAP_ID) {
      if (legsup & USBLEGSUP_BIOS_OWNED) {
        pci_write32(hc->pci_addr, eecp, legsup | USBLEGSUP_OS_OWNED);
        for (int elapsed = 0; elapsed < 1000; elapsed++) {
          legsup = pci_read32(hc->pci_addr, eecp);
          if (!(legsup & USBLEGSUP_BIOS_OWNED))
            break;
          pit_delay_ms(1);
        }
        if (legsup & USBLEGSUP_BIOS_OWNED) {
          log_write("EHCI: BIOS ownership handoff timed out", KERNEL,
                    LOG_ERROR);
          return -1;
        }
      } else if (!(legsup & USBLEGSUP_OS_OWNED)) {
        pci_write32(hc->pci_addr, eecp, legsup | USBLEGSUP_OS_OWNED);
      }
      pci_write32(hc->pci_addr, (u16)(eecp + USBLEGCTLSTS_OFFSET), 0);
      return 0;
    }
    if (!next || next == eecp)
      break;
    eecp = next;
  }
  return 0;
}

static struct ehci_qh *ehci_qh_at(struct ehci_hcd *hc, u32 offset) {
  return (struct ehci_qh *)(void *)(hc->arena + offset);
}
static struct ehci_qtd *ehci_qtd_at(struct ehci_hcd *hc, int index) {
  return (struct ehci_qtd *)(void *)(hc->arena + EHCI_ARENA_QTD_POOL +
                                     (usize)index * sizeof(struct ehci_qtd));
}
static u64 ehci_qtd_phys(struct ehci_hcd *hc, int index) {
  return hc->arena_phys + EHCI_ARENA_QTD_POOL +
         (u64)index * sizeof(struct ehci_qtd);
}

static void ehci_qtd_init(struct ehci_qtd *qtd, u64 next_phys,
                          u64 alt_phys, u32 pid, int toggle, u16 length,
                          u64 buffer_phys, int ioc) {
  memset(qtd, 0, sizeof(*qtd));
  qtd->next = next_phys ? (u32)next_phys : LINK_TERMINATE;
  qtd->alt_next = alt_phys ? (u32)alt_phys : LINK_TERMINATE;
  qtd->buf[0] = (u32)buffer_phys;
  if (length) {
    u64 page = (buffer_phys & ~0xFFFULL) + 0x1000;
    for (int i = 1; i < 5; i++, page += 0x1000)
      qtd->buf[i] = (u32)page;
  }
  u32 token = pid | QTD_STATUS_ACTIVE | FIELD_PREP(QTD_CERR_MASK, 3) |
              FIELD_PREP(QTD_TOTAL_LEN_MASK, length);
  if (toggle)
    token |= QTD_TOGGLE;
  if (ioc)
    token |= QTD_IOC;
  qtd_token_write(qtd, token);
}

/* Stop fetching before rewriting the shared QH/qTDs. If the controller cannot
 * quiesce, keep its permanent arena pinned and refuse further async requests. */
static int ehci_async_stop(struct ehci_hcd *hc) {
  ehci_op_write(hc, EHCI_USBCMD, ehci_op_read(hc, EHCI_USBCMD) &
                ~CMD_REG_ASYNC_SCHEDULE_ENABLE);
  if (ehci_wait_clear(hc->op, EHCI_USBSTS, STS_REG_ASYNC_SCHEDULE_STATUS, 100)) {
    hc->async_failed = 1;
    return -1;
  }
  ehci_dma_barrier();
  return 0;
}
static void ehci_async_start(struct ehci_hcd *hc) {
  ehci_dma_barrier();
  ehci_op_write(hc, EHCI_USBCMD, ehci_op_read(hc, EHCI_USBCMD) |
                CMD_REG_ASYNC_SCHEDULE_ENABLE);
}
static int ehci_transfer_error(u32 token) {
  return (token & QTD_ERROR_MASK) == QTD_STATUS_HALTED ? USB_STORAGE_STALL : -1;
}

/* Caller holds async_lock and has stopped the asynchronous schedule. */
static int ehci_ctrl_locked(struct ehci_hcd *hc, u8 addr, u16 maxpacket,
                          const struct usb_setup_packet *setup, void *data,
                          u16 length, u32 timeout_ms) {
  if (!hc || !setup || maxpacket == 0 || length > EHCI_ARENA_DATA_MAX ||
      setup->wLength != length)
    return -1;
  u8 *setup_buf = hc->arena + EHCI_ARENA_SETUP;
  u8 *data_buf = hc->arena + EHCI_ARENA_DATA;
  u64 setup_phys = hc->arena_phys + EHCI_ARENA_SETUP;
  u64 data_phys = hc->arena_phys + EHCI_ARENA_DATA;
  int dir_in = (setup->bmRequestType & 0x80) != 0;

  memcpy(setup_buf, setup, sizeof(*setup));
  if (length && !dir_in)
    memcpy(data_buf, data, length);
  else if (length)
    memset(data_buf, 0, length);

  int status_index = length ? 2 : 1;
  u64 status_phys = ehci_qtd_phys(hc, status_index);
  ehci_qtd_init(ehci_qtd_at(hc, 0), ehci_qtd_phys(hc, 1), 0,
                QTD_PID_SETUP, 0, sizeof(*setup), setup_phys, 0);
  if (length)
    ehci_qtd_init(ehci_qtd_at(hc, 1), status_phys, status_phys,
                  dir_in ? QTD_PID_IN : QTD_PID_OUT, 1, length, data_phys, 0);
  ehci_qtd_init(ehci_qtd_at(hc, status_index), 0, 0,
                dir_in ? QTD_PID_OUT : QTD_PID_IN, 1, 0, 0, 0);

  struct ehci_qh *qh = ehci_qh_at(hc, EHCI_ARENA_CONTROL_QH);
  qh->ep_char = FIELD_PREP(QH_EPCHAR_ADDR_MASK, addr) |
                QH_EPCHAR_SPEED_HIGH | QH_EPCHAR_DTC |
                FIELD_PREP(QH_EPCHAR_MAXP_MASK, maxpacket);
  qh->ep_cap = QH_EPCAP_MULT_1;
  qh->curr_qtd = 0;
  qh->overlay.alt_next = LINK_TERMINATE;
  qh->overlay.token = 0;
  memset(qh->overlay.buf, 0, sizeof(qh->overlay.buf));
  ehci_dma_barrier();
  qh_next_write(qh, (u32)ehci_qtd_phys(hc, 0));
  ehci_async_start(hc);

  int done = 0;
  int failed = 0;
  for (u64 elapsed = 0; elapsed < (u64)timeout_ms * 10; elapsed++) {
    for (int i = 0; i <= status_index; i++) {
      if (qtd_token_read(ehci_qtd_at(hc, i)) & QTD_ERROR_MASK) {
        failed = ehci_transfer_error(qtd_token_read(ehci_qtd_at(hc, i)));
        break;
      }
    }
    u32 status_token = qtd_token_read(ehci_qtd_at(hc, status_index));
    if (failed || !(status_token & QTD_STATUS_ACTIVE)) {
      done = !failed;
      break;
    }
    pit_delay_us(100);
  }

  u32 qh_after = qh_next_read(qh);
  qh_next_write(qh, LINK_TERMINATE);
  ehci_dma_barrier();
  if (!done) {
    log_write(failed ? "EHCI: control transfer failed"
                     : "EHCI: control transfer timed out",
              KERNEL, LOG_ERROR);
    log_write_hex("EHCI:   USBSTS", ehci_op_read(hc, EHCI_USBSTS), KERNEL,
                  LOG_ERROR);
    log_write_hex("EHCI:   QH next", qh_after, KERNEL, LOG_ERROR);
    for (int i = 0; i <= status_index; i++)
      log_write_hex("EHCI:   qTD token", qtd_token_read(ehci_qtd_at(hc, i)),
                    KERNEL, LOG_ERROR);
    return failed ? failed : -1;
  }

  if (!length)
    return 0;
  u32 data_token = qtd_token_read(ehci_qtd_at(hc, 1));
  u16 remaining = (u16)FIELD_GET(QTD_TOTAL_LEN_MASK, data_token);
  int transferred = length - remaining;
  if (dir_in)
    memcpy(data, data_buf, (usize)transferred);
  return transferred;
}

static int ehci_ctrl_xfer(struct ehci_hcd *hc, u8 addr, u16 maxpacket,
                          const struct usb_setup_packet *setup, void *data,
                          u16 length, u32 timeout_ms) {
  if (!hc || (length && !data)) return -1;
  spin_lock(&hc->async_lock);
  int rc = -1;
  if (!hc->async_failed && !ehci_async_stop(hc)) {
    rc = ehci_ctrl_locked(hc, addr, maxpacket, setup, data, length, timeout_ms);
    if (ehci_async_stop(hc)) rc = -1;
  }
  spin_unlock(&hc->async_lock);
  return rc;
}

static int ehci_storage_connected(struct ehci_storage_device *dev) {
  u32 status = ehci_op_read(dev->hc, EHCI_PORTSC(dev->port));
  u32 needed = PORTSC_REG_CONNECT_STATUS | PORTSC_REG_ENABLE;
  if ((status & needed) != needed || (status & PORTSC_REG_CONNECT_STATUS_CHANGE))
    dev->disconnected = 1; /* never direct a stale mount at a replacement disk */
  return !dev->disconnected;
}
static int ehci_storage_control(void *context,
                                const struct usb_setup_packet *setup, void *data) {
  struct ehci_storage_device *dev = context;
  if (!ehci_storage_connected(dev)) return -1;
  return ehci_ctrl_xfer(dev->hc, dev->address, dev->maxpacket, setup, data,
                        setup->wLength, EHCI_XFER_TIMEOUT_MS);
}

static int ehci_storage_bulk(void *context, u8 endpoint, u16 packet, u8 *toggle,
                             void *buffer, u32 length, u32 *actual) {
  struct ehci_storage_device *dev = context;
  struct ehci_hcd *hc = dev->hc;
  *actual = 0;
  if (!buffer || !length || packet != 512) return -1;
  spin_lock(&hc->async_lock);
  int rc = -1;
  if (hc->async_failed || !ehci_storage_connected(dev) || ehci_async_stop(hc))
    goto out;
  u8 *bounce = hc->arena + EHCI_ARENA_BULK_BUF;
  struct ehci_qh *qh = ehci_qh_at(hc, EHCI_ARENA_CONTROL_QH);
  struct ehci_qtd *qtd = ehci_qtd_at(hc, 0);
  int input = (endpoint & 0x80) != 0;
  while (*actual < length) {
    u32 chunk = length - *actual;
    if (chunk > EHCI_ARENA_BULK_MAX) chunk = EHCI_ARENA_BULK_MAX;
    if (!input) memcpy(bounce, (u8 *)buffer + *actual, chunk);
    ehci_qtd_init(qtd, 0, 0, input ? QTD_PID_IN : QTD_PID_OUT, *toggle,
                  chunk, hc->arena_phys + EHCI_ARENA_BULK_BUF, 0);
    qh->ep_char = FIELD_PREP(QH_EPCHAR_ADDR_MASK, dev->address) |
                  FIELD_PREP(QH_EPCHAR_EP_MASK, endpoint & 0x0f) |
                  QH_EPCHAR_SPEED_HIGH | QH_EPCHAR_DTC |
                  FIELD_PREP(QH_EPCHAR_MAXP_MASK, packet);
    qh->ep_cap = QH_EPCAP_MULT_1;
    qh->curr_qtd = 0;
    memset(&qh->overlay, 0, sizeof(qh->overlay));
    qh->overlay.alt_next = LINK_TERMINATE;
    qh->overlay.next = (u32)ehci_qtd_phys(hc, 0);
    ehci_async_start(hc);
    u32 token = QTD_STATUS_ACTIVE;
    for (u32 elapsed = 0; elapsed < EHCI_XFER_TIMEOUT_MS * 10; elapsed++) {
      token = qtd_token_read(qtd);
      if (!(token & QTD_STATUS_ACTIVE) || (token & QTD_ERROR_MASK)) break;
      pit_delay_us(100);
    }
    if (ehci_async_stop(hc)) goto out;
    token = qtd_token_read(qtd);
    qh_next_write(qh, LINK_TERMINATE);
    u32 remaining = FIELD_GET(QTD_TOTAL_LEN_MASK, token);
    if (remaining > chunk) goto out;
    u32 done = chunk - remaining;
    if (input) memcpy((u8 *)buffer + *actual, bounce, done);
    *actual += done;
    if (token & QTD_ERROR_MASK) {
      rc = ehci_transfer_error(token);
      goto out;
    }
    if (token & QTD_STATUS_ACTIVE) goto out;
    /* A short transfer ending on a packet boundary includes a zero packet. */
    u32 packets = (done + packet - 1) / packet;
    if (done < chunk && done % packet == 0) packets++;
    *toggle ^= packets & 1;
    if (done < chunk) break;
  }
  rc = 0;
out:
  spin_unlock(&hc->async_lock);
  return rc;
}

static int ehci_get_descriptor(struct ehci_hcd *hc, u8 addr, u16 maxpacket,
                               u8 type, u8 index, void *out, u16 length) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_IN_STD_DEVICE,
      .bRequest = USB_REQ_GET_DESCRIPTOR,
      .wValue = (u16)((type << 8) | index),
      .wIndex = 0,
      .wLength = length,
  };
  return ehci_ctrl_xfer(hc, addr, maxpacket, &setup, out, length,
                        EHCI_XFER_TIMEOUT_MS);
}
static int ehci_set_address(struct ehci_hcd *hc, u16 maxpacket, u8 address) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_OUT_STD_DEVICE,
      .bRequest = USB_REQ_SET_ADDRESS,
      .wValue = address,
      .wLength = 0,
  };
  return ehci_ctrl_xfer(hc, 0, maxpacket, &setup, 0, 0,
                        EHCI_XFER_TIMEOUT_MS);
}
static int ehci_set_configuration(struct ehci_hcd *hc, u8 addr,
                                  u16 maxpacket, u8 configuration) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_OUT_STD_DEVICE,
      .bRequest = USB_REQ_SET_CONFIGURATION,
      .wValue = configuration,
      .wLength = 0,
  };
  return ehci_ctrl_xfer(hc, addr, maxpacket, &setup, 0, 0,
                        EHCI_XFER_TIMEOUT_MS);
}
static int ehci_hid_request(struct ehci_hcd *hc, u8 addr, u16 maxpacket,
                            u8 request, u16 value, u8 interface) {
  struct usb_setup_packet setup = {
      .bmRequestType = USB_REQTYPE_OUT_CLASS_INTERFACE,
      .bRequest = request,
      .wValue = value,
      .wIndex = interface,
      .wLength = 0,
  };
  return ehci_ctrl_xfer(hc, addr, maxpacket, &setup, 0, 0,
                        EHCI_XFER_TIMEOUT_MS);
}

static int ehci_read_config(struct ehci_hcd *hc, u8 addr, u16 maxpacket,
                            u8 index) {
  struct usb_config_descriptor head;
  memset(&head, 0, sizeof(head));
  if (ehci_get_descriptor(hc, addr, maxpacket, USB_DESC_CONFIGURATION, index,
                          &head, sizeof(head)) < (int)sizeof(head))
    return -1;
  u16 total = head.wTotalLength;
  if (total < sizeof(head))
    return -1;
  if (total > EHCI_CONFIG_BUF_MAX) {
    log_write_int("EHCI: configuration too large, truncating", total, KERNEL,
                  LOG_WARN);
    total = EHCI_CONFIG_BUF_MAX;
  }
  memset(ehci_config_buf, 0, sizeof(ehci_config_buf));
  int got = ehci_get_descriptor(hc, addr, maxpacket, USB_DESC_CONFIGURATION,
                                index, ehci_config_buf, total);
  return got >= (int)sizeof(head) ? got : -1;
}

static int ehci_find_hid_pointer(const u8 *buf, int length,
                                 struct ehci_hid_pointer *out) {
  int found = 0;
  int hid_interface = 0;
  u8 current_interface = 0;
  u8 current_kind = EHCI_INT_KIND_NONE;
  for (int offset = 0; offset + 2 <= length;) {
    u8 descriptor_length = buf[offset];
    u8 descriptor_type = buf[offset + 1];
    if (descriptor_length < 2 || offset + descriptor_length > length)
      break;
    if (descriptor_type == USB_DESC_INTERFACE &&
        descriptor_length >= sizeof(struct usb_interface_descriptor)) {
      const struct usb_interface_descriptor *interface =
          (const struct usb_interface_descriptor *)(const void *)(buf + offset);
      current_interface = interface->bInterfaceNumber;
      hid_interface = interface->bInterfaceClass == USB_CLASS_HID;
      current_kind = EHCI_INT_KIND_NONE;
      if (hid_interface &&
          interface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
          interface->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE)
        current_kind = EHCI_INT_KIND_HID_BOOT_MOUSE;
      log_write_int("EHCI:   interface", current_interface, KERNEL, LOG_INFO);
      log_write_hex("EHCI:     class", interface->bInterfaceClass, KERNEL,
                    LOG_INFO);
      log_write_hex("EHCI:     protocol", interface->bInterfaceProtocol,
                    KERNEL, LOG_INFO);
    } else if (descriptor_type == USB_DESC_HID && descriptor_length >= 9) {
      u16 report_length = (u16)buf[offset + 7] |
                          ((u16)buf[offset + 8] << 8);
      if (hid_interface && current_kind == EHCI_INT_KIND_NONE &&
          report_length == 74)
        current_kind = EHCI_INT_KIND_HID_TABLET;
    } else if (descriptor_type == USB_DESC_ENDPOINT &&
               descriptor_length >= sizeof(struct usb_endpoint_descriptor)) {
      const struct usb_endpoint_descriptor *endpoint =
          (const struct usb_endpoint_descriptor *)(const void *)(buf + offset);
      u16 maxpacket = endpoint->wMaxPacketSize & 0x07FF;
      log_write_hex("EHCI:   endpoint", endpoint->bEndpointAddress, KERNEL,
                    LOG_INFO);
      log_write_int("EHCI:     max packet", maxpacket, KERNEL, LOG_INFO);
      int size_ok = current_kind != EHCI_INT_KIND_HID_TABLET || maxpacket >= 5;
      if (!found && current_kind != EHCI_INT_KIND_NONE && size_ok &&
          maxpacket <= EHCI_INT_BUF_MAX &&
          (endpoint->bmAttributes & USB_EP_XFER_MASK) ==
              USB_EP_XFER_INTERRUPT &&
          (endpoint->bEndpointAddress & USB_EP_ADDR_DIR_IN)) {
        out->interface_number = current_interface;
        out->kind = current_kind;
        out->endpoint = *endpoint;
        out->endpoint.wMaxPacketSize = maxpacket;
        found = 1;
      }
    }
    offset += descriptor_length;
  }
  return found;
}

/* PORTSC has write-one-to-clear change bits. Strip them from ordinary RMWs. */
static void ehci_port_set(struct ehci_hcd *hc, int port, u32 bits) {
  u32 reg = EHCI_PORTSC(port);
  u32 value = ehci_op_read(hc, reg);
  ehci_op_write(hc, reg, (value & ~PORTSC_REG_WC_MASK) | bits);
}
static void ehci_port_clear(struct ehci_hcd *hc, int port, u32 bits) {
  u32 reg = EHCI_PORTSC(port);
  u32 value = ehci_op_read(hc, reg);
  ehci_op_write(hc, reg, (value & ~PORTSC_REG_WC_MASK) & ~bits);
}
static void ehci_port_ack_changes(struct ehci_hcd *hc, int port) {
  u32 reg = EHCI_PORTSC(port);
  u32 value = ehci_op_read(hc, reg);
  if (value & PORTSC_REG_WC_MASK)
    ehci_op_write(hc, reg, value);
}

/* Returns 0 only for a connected high-speed device. */
static int ehci_port_reset(struct ehci_hcd *hc, int port) {
  u32 reg = EHCI_PORTSC(port);
  u32 status = ehci_op_read(hc, reg);
  if (!(status & PORTSC_REG_CONNECT_STATUS))
    return -1;
  if (hc->port_power_control && !(status & PORTSC_REG_PORT_POWER)) {
    ehci_port_set(hc, port, PORTSC_REG_PORT_POWER);
    pit_delay_ms(20);
  }
  ehci_port_set(hc, port, PORTSC_REG_RESET);
  pit_delay_ms(50);
  ehci_port_clear(hc, port, PORTSC_REG_RESET);
  if (ehci_wait_clear(hc->op, reg, PORTSC_REG_RESET, 100) != 0)
    return -1;
  pit_delay_ms(2);
  status = ehci_op_read(hc, reg);
  ehci_port_ack_changes(hc, port);
  if (!(status & PORTSC_REG_CONNECT_STATUS))
    return -1;
  if (!(status & PORTSC_REG_ENABLE)) {
    ehci_port_set(hc, port, PORTSC_REG_OWNER);
    log_write_int("EHCI: port routed to companion", port, KERNEL, LOG_INFO);
    return -1;
  }
  return 0;
}

static void ehci_periodic_arm(struct ehci_int_ep *ep) {
  memset(ep->buf, 0, ep->maxlen);
  ehci_qtd_init(ep->qtd, 0, 0, QTD_PID_IN, ep->toggle, ep->maxlen,
                ep->buf_phys, 1);
  ep->qh->curr_qtd = 0;
  ep->qh->overlay.alt_next = LINK_TERMINATE;
  ep->qh->overlay.token = 0;
  memset(ep->qh->overlay.buf, 0, sizeof(ep->qh->overlay.buf));
  ehci_dma_barrier();
  qh_next_write(ep->qh, (u32)ep->qtd_phys);
  ehci_dma_barrier();
}

static int ehci_periodic_start(struct ehci_hcd *hc, u8 addr, u8 ep,
                               u16 maxlen, u8 interval, u8 kind) {
  struct ehci_int_ep *iep = &hc->int_ep;
  if (iep->active || maxlen == 0 || maxlen > EHCI_INT_BUF_MAX)
    return -1;
  iep->addr = addr;
  iep->ep = ep;
  iep->maxlen = maxlen;
  iep->interval = interval;
  iep->kind = kind;
  iep->qh = ehci_qh_at(hc, EHCI_ARENA_PERIODIC_QH);
  iep->qh_phys = hc->arena_phys + EHCI_ARENA_PERIODIC_QH;
  iep->qtd = (struct ehci_qtd *)(void *)(hc->arena +
                                         EHCI_ARENA_PERIODIC_QTD);
  iep->qtd_phys = hc->arena_phys + EHCI_ARENA_PERIODIC_QTD;
  iep->buf = hc->arena + EHCI_ARENA_INT_BUF;
  iep->buf_phys = hc->arena_phys + EHCI_ARENA_INT_BUF;
  memset(iep->qh, 0, sizeof(*iep->qh));
  iep->qh->horiz_link = LINK_TERMINATE;
  iep->qh->ep_char = FIELD_PREP(QH_EPCHAR_ADDR_MASK, addr) |
                     FIELD_PREP(QH_EPCHAR_EP_MASK, ep) |
                     QH_EPCHAR_SPEED_HIGH | QH_EPCHAR_DTC |
                     FIELD_PREP(QH_EPCHAR_MAXP_MASK, maxlen);
  /* High-speed bInterval is an exponent in 125 us microframes. Express
   * sub-millisecond periods with S-mask and longer ones with frame spacing. */
  u8 smask = 1;
  u16 frame_stride = 1;
  if (interval <= 1)
    smask = 0xFF;
  else if (interval == 2)
    smask = 0x55;
  else if (interval == 3)
    smask = 0x11;
  else if (interval > 4)
    frame_stride = interval >= 14 ? 1024 : (u16)(1U << (interval - 4));
  iep->qh->ep_cap = FIELD_PREP(QH_EPCAP_SMASK_MASK, smask) | QH_EPCAP_MULT_1;
  iep->active = 1;
  ehci_periodic_arm(iep);
  u32 qh_link = (u32)iep->qh_phys | LINK_TYPE_QH;
  for (int i = 0; i < 1024; i++)
    hc->periodic[i] = (i % frame_stride) == 0 ? qh_link : LINK_TERMINATE;
  ehci_dma_barrier();

  u32 cmd = ehci_op_read(hc, EHCI_USBCMD);
  ehci_op_write(hc, EHCI_USBCMD, cmd | CMD_REG_PERIODIC_SCHEDULE_ENABLE);
  if (ehci_wait_set(hc->op, EHCI_USBSTS,
                    STS_REG_PERIODIC_SCHEDULE_STATUS, 100) != 0) {
    iep->active = 0;
    ehci_op_write(hc, EHCI_USBCMD, cmd);
    return -1;
  }
  return 0;
}

static void ehci_periodic_complete(struct ehci_int_ep *ep) {
  if (!ep->active)
    return;
  u32 token = qtd_token_read(ep->qtd);
  if (token & QTD_STATUS_ACTIVE)
    return;
  if (token & QTD_ERROR_MASK) {
    ep->errors++;
    if (ep->errors <= EHCI_INT_LOG_LIMIT)
      log_write_hex("EHCI: periodic transfer error", token, KERNEL, LOG_WARN);
  } else {
    u16 remaining = (u16)FIELD_GET(QTD_TOTAL_LEN_MASK, token);
    ep->last_len = ep->maxlen - remaining;
    ep->reports++;
    ep->toggle ^= 1;
    if (ep->kind == EHCI_INT_KIND_HID_BOOT_MOUSE)
      mouse_hid_report(ep->buf, ep->last_len);
    else if (ep->kind == EHCI_INT_KIND_HID_TABLET)
      mouse_hid_tablet_report(ep->buf, ep->last_len);
    if (ep->reports <= EHCI_INT_LOG_LIMIT)
      log_write_int("EHCI: report bytes", ep->last_len, KERNEL, LOG_INFO);
  }
  ehci_periodic_arm(ep);
}

static void ehci_irq_handler(void) {
  for (int i = 0; i < ehci_controller_count; i++) {
    struct ehci_hcd *hc = &ehci_controllers[i];
    if (!hc->op)
      continue;
    u32 pending = ehci_op_read(hc, EHCI_USBSTS) & 0x3F;
    if (!pending)
      continue;
    ehci_op_write(hc, EHCI_USBSTS, pending);
    if (pending & (STS_REG_INTERRUPT | STS_REG_ERROR_INTERRUPT))
      ehci_periodic_complete(&hc->int_ep);
    if (pending & STS_REG_HOST_SYSTEM_ERROR)
      log_write("EHCI: host system error", KERNEL, LOG_ERROR);
  }
}

static void ehci_enumerate_port(struct ehci_hcd *hc, int port) {
  struct usb_descriptor descriptor;
  if (ehci_port_reset(hc, port) != 0)
    return;
  log_write_int("EHCI: high-speed port enabled", port, KERNEL, LOG_INFO);
  memset(&descriptor, 0, sizeof(descriptor));
  if (ehci_get_descriptor(hc, 0, EHCI_EP0_DEFAULT_MAXP, USB_DESC_DEVICE, 0,
                          &descriptor, 8) < 8) {
    log_write_int("EHCI: initial descriptor failed on port", port, KERNEL,
                  LOG_ERROR);
    return;
  }
  u16 maxpacket = descriptor.bMaxPacketSize0;
  if (maxpacket == 0)
    maxpacket = EHCI_EP0_DEFAULT_MAXP;
  if (ehci_port_reset(hc, port) != 0)
    return;
  u8 address = hc->next_addr;
  if (ehci_set_address(hc, maxpacket, address) < 0) {
    log_write_int("EHCI: SET_ADDRESS failed on port", port, KERNEL, LOG_ERROR);
    return;
  }
  hc->next_addr++;
  pit_delay_ms(2);
  memset(&descriptor, 0, sizeof(descriptor));
  if (ehci_get_descriptor(hc, address, maxpacket, USB_DESC_DEVICE, 0,
                          &descriptor, sizeof(descriptor)) <
      (int)sizeof(descriptor)) {
    log_write_int("EHCI: full descriptor failed for address", address,
                  KERNEL, LOG_ERROR);
    return;
  }
  log_write_int("EHCI: device addressed", address, KERNEL, LOG_INFO);
  log_write_hex("EHCI:   idVendor", descriptor.idVendor, KERNEL, LOG_INFO);
  log_write_hex("EHCI:   idProduct", descriptor.idProduct, KERNEL, LOG_INFO);
  log_write_hex("EHCI:   bcdUSB", descriptor.bcdUSB, KERNEL, LOG_INFO);
  if (descriptor.bNumConfigurations == 0)
    return;

  int config_length = ehci_read_config(hc, address, maxpacket, 0);
  if (config_length < 0)
    return;
  const struct usb_config_descriptor *config =
      (const struct usb_config_descriptor *)(const void *)ehci_config_buf;
  struct ehci_hid_pointer pointer;
  memset(&pointer, 0, sizeof(pointer));
  int have_pointer =
      ehci_find_hid_pointer(ehci_config_buf, config_length, &pointer);
  if (ehci_set_configuration(hc, address, maxpacket,
                             config->bConfigurationValue) < 0) {
    log_write_int("EHCI: SET_CONFIGURATION failed for address", address,
                  KERNEL, LOG_ERROR);
    return;
  }
  log_write_int("EHCI: device configured", config->bConfigurationValue,
                KERNEL, LOG_INFO);
  struct ehci_storage_device *disk = &hc->storage[port];
  *disk = (struct ehci_storage_device){
      .hc = hc, .address = address, .port = port, .maxpacket = maxpacket,
  };
  struct usb_storage_transport transport = {
      .context = disk, .control = ehci_storage_control, .bulk = ehci_storage_bulk,
  };
  if (usb_storage_probe(&transport, ehci_config_buf, config_length) == 0)
    log_write("USB storage: SCSI/BOT disk ready", KERNEL, LOG_INFO);
  if (!have_pointer)
    return;
  if (ehci_hid_request(hc, address, maxpacket, USB_REQ_SET_IDLE, 0,
                       pointer.interface_number) < 0)
    log_write("EHCI: HID SET_IDLE failed, continuing", KERNEL, LOG_WARN);
  if (pointer.kind == EHCI_INT_KIND_HID_BOOT_MOUSE &&
      ehci_hid_request(hc, address, maxpacket, USB_REQ_SET_PROTOCOL, 0,
                       pointer.interface_number) < 0)
    log_write("EHCI: HID boot protocol failed, continuing", KERNEL, LOG_WARN);
  u8 endpoint = pointer.endpoint.bEndpointAddress & USB_EP_ADDR_NUM_MASK;
  if (ehci_periodic_start(hc, address, endpoint,
                          pointer.endpoint.wMaxPacketSize,
                          pointer.endpoint.bInterval, pointer.kind) == 0)
    log_write_int("EHCI: polling HID pointer endpoint", endpoint, KERNEL,
                  LOG_INFO);
}

static void ehci_scan_ports(struct ehci_hcd *hc) {
  for (int port = 0; port < hc->ports; port++) {
    u32 status = ehci_op_read(hc, EHCI_PORTSC(port));
    log_write_int("EHCI: scanning port", port, KERNEL, LOG_INFO);
    log_write_hex("EHCI:   PORTSC", status, KERNEL, LOG_INFO);
    if (status & PORTSC_REG_CONNECT_STATUS)
      ehci_enumerate_port(hc, port);
  }
}

static void ehci_async_schedule_init(struct ehci_hcd *hc) {
  struct ehci_qh *head = ehci_qh_at(hc, EHCI_ARENA_ASYNC_HEAD);
  struct ehci_qh *control = ehci_qh_at(hc, EHCI_ARENA_CONTROL_QH);
  u64 head_phys = hc->arena_phys + EHCI_ARENA_ASYNC_HEAD;
  u64 control_phys = hc->arena_phys + EHCI_ARENA_CONTROL_QH;
  memset(head, 0, sizeof(*head));
  memset(control, 0, sizeof(*control));
  head->horiz_link = (u32)control_phys | LINK_TYPE_QH;
  head->ep_char = QH_EPCHAR_H;
  head->ep_cap = QH_EPCAP_MULT_1;
  head->overlay.next = LINK_TERMINATE;
  head->overlay.alt_next = LINK_TERMINATE;
  control->horiz_link = (u32)head_phys | LINK_TYPE_QH;
  control->ep_char = QH_EPCHAR_SPEED_HIGH | QH_EPCHAR_DTC |
                     FIELD_PREP(QH_EPCHAR_MAXP_MASK, EHCI_EP0_DEFAULT_MAXP);
  control->ep_cap = QH_EPCAP_MULT_1;
  control->overlay.next = LINK_TERMINATE;
  control->overlay.alt_next = LINK_TERMINATE;
}

static void ehci_free_dma(struct ehci_hcd *hc) {
  if (hc->arena_phys) {
    pmm_free_frame(hc->arena_phys);
    hc->arena_phys = 0;
  }
  if (hc->periodic_phys) {
    pmm_free_frame(hc->periodic_phys);
    hc->periodic_phys = 0;
  }
}

int ehci_init(struct pci_device *dev) {
  if (!dev || ehci_controller_count >= EHCI_MAX_CONTROLLERS)
    return -1;
  if (!dev->bar[0].valid || dev->bar[0].is_io) {
    log_write("EHCI: BAR0 is not MMIO", KERNEL, LOG_ERROR);
    return -1;
  }
  struct ehci_hcd *hc = &ehci_controllers[ehci_controller_count];
  memset(hc, 0, sizeof(*hc));
  hc->pci_addr = dev->addr;
  hc->irq = dev->int_line;
  hc->next_addr = 1;
  hc->mmio = ehci_map_mmio(&dev->bar[0], &hc->mmio_page, &hc->mmio_pages);
  if (!hc->mmio) {
    log_write("EHCI: could not map BAR0", KERNEL, LOG_ERROR);
    return -1;
  }

  u8 caplength = ehci_read8(hc->mmio + EHCI_CAPLENGTH);
  u16 version = ehci_read16(hc->mmio + EHCI_HCIVERSION);
  u32 hcsparams = ehci_read32(hc->mmio + EHCI_HCSPARAMS);
  u32 hccparams = ehci_read32(hc->mmio + EHCI_HCCPARAMS);
  if (caplength < 0x10 || caplength >= dev->bar[0].size) {
    log_write("EHCI: invalid CAPLENGTH", KERNEL, LOG_ERROR);
    goto fail_unmap;
  }
  hc->op = hc->mmio + caplength;
  hc->ports = (u8)FIELD_GET(HCSPARAMS_REG_N_PORTS, hcsparams);
  hc->port_power_control = (hcsparams & HCSPARAMS_REG_PPC) != 0;
  if (hc->ports == 0 || hc->ports > 15) {
    log_write("EHCI: invalid root-port count", KERNEL, LOG_ERROR);
    goto fail_unmap;
  }
  log_write("EHCI: initializing", KERNEL, LOG_INFO);
  log_write_hex("EHCI: MMIO", hc->mmio, KERNEL, LOG_INFO);
  log_write_hex("EHCI: version", version, KERNEL, LOG_INFO);
  log_write_int("EHCI: root ports", hc->ports, KERNEL, LOG_INFO);
  log_write_int("EHCI: IRQ line", hc->irq, KERNEL, LOG_INFO);
  if (ehci_take_ownership(hc, hccparams) != 0)
    goto fail_unmap;

  hc->periodic_phys = pmm_alloc_frame_below(EHCI_DMA_LIMIT);
  hc->arena_phys = pmm_alloc_frame_below(EHCI_DMA_LIMIT);
  if (!hc->periodic_phys || !hc->arena_phys)
    goto fail_dma;
  hc->periodic = (u32 *)phys_to_virt(hc->periodic_phys);
  hc->arena = (u8 *)phys_to_virt(hc->arena_phys);
  memset(hc->arena, 0, FRAME_SIZE);
  for (int i = 0; i < 1024; i++)
    hc->periodic[i] = LINK_TERMINATE;
  ehci_async_schedule_init(hc);
  ehci_dma_barrier();

  u32 cmd = ehci_op_read(hc, EHCI_USBCMD) &
            ~(CMD_REG_RUN_STOP | CMD_REG_ASYNC_SCHEDULE_ENABLE |
              CMD_REG_PERIODIC_SCHEDULE_ENABLE);
  ehci_op_write(hc, EHCI_USBCMD, cmd);
  if (ehci_wait_set(hc->op, EHCI_USBSTS, STS_REG_HOST_CONTROLLER_HALTED,
                    100) != 0) {
    log_write("EHCI: controller did not halt", KERNEL, LOG_ERROR);
    goto fail_dma;
  }
  ehci_op_write(hc, EHCI_USBCMD, CMD_REG_HOST_CONTROLLER_RESET);
  if (ehci_wait_clear(hc->op, EHCI_USBCMD,
                      CMD_REG_HOST_CONTROLLER_RESET, 100) != 0) {
    log_write("EHCI: reset did not complete", KERNEL, LOG_ERROR);
    goto fail_dma;
  }
  ehci_op_write(hc, EHCI_CTRLDSSEGMENT, 0);
  ehci_op_write(hc, EHCI_PERIODICLISTBASE, (u32)hc->periodic_phys);
  ehci_op_write(hc, EHCI_ASYNCLISTADDR,
                (u32)(hc->arena_phys + EHCI_ARENA_ASYNC_HEAD));
  ehci_op_write(hc, EHCI_USBINTR, 0);
  ehci_op_write(hc, EHCI_USBSTS, 0x3F);
  ehci_op_write(hc, EHCI_CONFIGFLAG, CONFIGFLAG_REG_CONFIGURED);
  cmd = CMD_REG_RUN_STOP | CMD_REG_ASYNC_SCHEDULE_ENABLE |
        FIELD_PREP(CMD_REG_INTERRUPT_THRESHOLD, 8);
  ehci_op_write(hc, EHCI_USBCMD, cmd);
  if (ehci_wait_clear(hc->op, EHCI_USBSTS,
                      STS_REG_HOST_CONTROLLER_HALTED, 100) != 0 ||
      ehci_wait_set(hc->op, EHCI_USBSTS,
                    STS_REG_ASYNC_SCHEDULE_STATUS, 100) != 0) {
    log_write("EHCI: schedules did not start", KERNEL, LOG_ERROR);
    goto fail_stop;
  }

  log_write("EHCI: controller started", KERNEL, LOG_INFO);
  ehci_controller_count++;
  pit_delay_ms(20);
  ehci_scan_ports(hc);
  if (hc->int_ep.active) {
    irq_install(hc->irq, ehci_irq_handler);
    pic_clear_mask(hc->irq);
    ehci_op_write(hc, EHCI_USBINTR,
                  INTR_REG_INTERRUPT_ENABLE |
                      INTR_REG_ERROR_INTERRUPT_ENABLE |
                      INTR_REG_HOST_SYSTEM_ERROR_ENABLE);
    log_write_int("EHCI: interrupts enabled on IRQ", hc->irq, KERNEL,
                  LOG_INFO);
  }
  return 0;

fail_stop:
  ehci_op_write(hc, EHCI_USBCMD, 0);
  ehci_wait_set(hc->op, EHCI_USBSTS, STS_REG_HOST_CONTROLLER_HALTED, 100);
fail_dma:
  ehci_free_dma(hc);
fail_unmap:
  ehci_unmap_mmio(hc);
  return -1;
}
