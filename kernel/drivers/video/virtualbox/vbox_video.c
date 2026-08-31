/* VirtualBox automatic display resizing.
 *
 * VirtualBox's UEFI GOP is a boot-time framebuffer only. After GRUB calls
 * ExitBootServices, window-size changes arrive through the VMMDev PCI device;
 * the guest must then set a new mode on the display controller. VirtualBox's
 * default Linux controller is VMSVGA, a VMware SVGA II compatible device.
 *
 * This driver deliberately implements only the small 2D subset TOS needs:
 * VMMDev graphics-capability reporting and display hints, SVGA mode registers,
 * and SVGA_CMD_UPDATE through the command FIFO. No 3D or screen objects are
 * involved.
 */
#include <devices/io.h>
#include <drivers/video/virtualbox/vbox_video.h>
#include <drivers/base/vendors/pci_ids.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <pci/pci.h>
#include <stdint.h>
#include <utilities/log.h>
#include <utilities/string.h>

#define VMMDEV_REQUEST_HEADER_VERSION 0x00010001U
#define VMMDEV_INTERFACE_VERSION 0x00010004U
#define VMMDEV_REQUEST_REPORT_GUEST_INFO 50U
#define VMMDEV_REQUEST_GET_DISPLAY_CHANGE2 54U
#define VMMDEV_REQUEST_REPORT_GUEST_CAPS 55U
#define VMMDEV_EVENT_DISPLAY_CHANGE (1U << 2)
#define VMMDEV_GUEST_SUPPORTS_GRAPHICS (1U << 2)
#define VMMDEV_OSTYPE_LINUX26_X64 0x00053100U
#define VMMDEV_REQUESTOR_DRIVER 1U

#define SVGA_MAGIC 0x00900000U
#define SVGA_ID_2 ((SVGA_MAGIC << 8) | 2U)
#define SVGA_REG_ID 0U
#define SVGA_REG_ENABLE 1U
#define SVGA_REG_WIDTH 2U
#define SVGA_REG_HEIGHT 3U
#define SVGA_REG_MAX_WIDTH 4U
#define SVGA_REG_MAX_HEIGHT 5U
#define SVGA_REG_DEPTH 6U
#define SVGA_REG_BITS_PER_PIXEL 7U
#define SVGA_REG_PSEUDOCOLOR 8U
#define SVGA_REG_RED_MASK 9U
#define SVGA_REG_GREEN_MASK 10U
#define SVGA_REG_BLUE_MASK 11U
#define SVGA_REG_BYTES_PER_LINE 12U
#define SVGA_REG_FB_OFFSET 14U
#define SVGA_REG_MEM_SIZE 19U
#define SVGA_REG_CONFIG_DONE 20U
#define SVGA_REG_SYNC 21U

#define SVGA_FIFO_MIN 0U
#define SVGA_FIFO_MAX 1U
#define SVGA_FIFO_NEXT_CMD 2U
#define SVGA_FIFO_STOP 3U
#define SVGA_FIFO_REGISTER_BYTES 4096U
#define SVGA_CMD_UPDATE 1U

#define VBOX_FIFO_VIRT_BASE 0xFFFFE20000000000ULL
#define VBOX_FIFO_MAX_BYTES (16ULL * 1024ULL * 1024ULL)

struct PACKED vmmdev_request_header {
  u32 size;
  u32 version;
  u32 request_type;
  i32 rc;
  u32 reserved1;
  u32 requestor;
};

struct PACKED vmmdev_guest_info {
  struct vmmdev_request_header header;
  u32 interface_version;
  u32 os_type;
};

struct PACKED vmmdev_guest_caps {
  struct vmmdev_request_header header;
  u32 caps;
};

struct PACKED vmmdev_display_change2 {
  struct vmmdev_request_header header;
  u32 xres;
  u32 yres;
  u32 bpp;
  u32 event_ack;
  u32 display;
};

_Static_assert(sizeof(struct vmmdev_request_header) == 24,
               "VMMDev request header ABI");
_Static_assert(sizeof(struct vmmdev_guest_info) == 32,
               "VMMDev guest info ABI");
_Static_assert(sizeof(struct vmmdev_guest_caps) == 28,
               "VMMDev guest caps ABI");
_Static_assert(sizeof(struct vmmdev_display_change2) == 44,
               "VMMDev display change v2 ABI");

static struct pci_device svga_device;
static u16 svga_io_base;
static volatile u32 *svga_fifo;
static u32 svga_fifo_bytes;
static u32 svga_fifo_mapped_pages;
static u16 vmmdev_port;
static u64 vmmdev_request_phys;
static void *vmmdev_request;
static int driver_active;

static u32 svga_read(u32 index) {
  outl(svga_io_base, index);
  return inl((u16)(svga_io_base + 1));
}

static void svga_write(u32 index, u32 value) {
  outl(svga_io_base, index);
  outl((u16)(svga_io_base + 1), value);
}

static void request_init(struct vmmdev_request_header *header, u32 size,
                         u32 type) {
  memset(vmmdev_request, 0, 4096);
  header->size = size;
  header->version = VMMDEV_REQUEST_HEADER_VERSION;
  header->request_type = type;
  header->rc = -1;
  header->requestor = VMMDEV_REQUESTOR_DRIVER;
}

static int vmmdev_perform(struct vmmdev_request_header *header) {
  __asm__ volatile("" ::: "memory");
  outl(vmmdev_port, (u32)vmmdev_request_phys);
  __asm__ volatile("" ::: "memory");
  return header->rc >= 0 ? 0 : -1;
}

static int map_fifo(u64 physical, u32 bytes) {
  if ((physical & 4095ULL) != 0 || bytes < SVGA_FIFO_REGISTER_BYTES + 20U ||
      bytes > VBOX_FIFO_MAX_BYTES)
    return -1;

  u32 pages = (bytes + 4095U) / 4096U;
  for (u32 i = 0; i < pages; i++) {
    if (vmm_map(VBOX_FIFO_VIRT_BASE + (u64)i * 4096ULL,
                physical + (u64)i * 4096ULL,
                VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT) != 0) {
      while (i > 0) {
        i--;
        vmm_unmap(VBOX_FIFO_VIRT_BASE + (u64)i * 4096ULL);
      }
      return -1;
    }
  }
  svga_fifo = (volatile u32 *)VBOX_FIFO_VIRT_BASE;
  svga_fifo_mapped_pages = pages;
  return 0;
}

static void unmap_fifo(void) {
  for (u32 i = 0; i < svga_fifo_mapped_pages; i++)
    vmm_unmap(VBOX_FIFO_VIRT_BASE + (u64)i * 4096ULL);
  svga_fifo = 0;
  svga_fifo_mapped_pages = 0;
}

static int fifo_submit(const u32 *words, u32 count) {
  u32 minimum = svga_fifo[SVGA_FIFO_MIN];
  u32 maximum = svga_fifo[SVGA_FIFO_MAX];
  u32 next = svga_fifo[SVGA_FIFO_NEXT_CMD];
  if (minimum < 16U || maximum > svga_fifo_bytes || next < minimum ||
      next >= maximum || count == 0 ||
      (u64)count * 4ULL >= maximum - minimum)
    return -1;

  /* Commit the whole packet with one NEXT_CMD update. Publishing one word at
   * a time lets the host observe an incomplete variable-length command. The
   * simple 2D path keeps at most one packet queued, so first drain the prior
   * packet and then reserve the now-empty ring without partial visibility. */
  svga_write(SVGA_REG_SYNC, 1);
  u32 spins = 1000000U;
  while (svga_fifo[SVGA_FIFO_STOP] != next && spins--)
    __asm__ volatile("pause");
  if (svga_fifo[SVGA_FIFO_STOP] != next)
    return -1;

  for (u32 i = 0; i < count; i++) {
    svga_fifo[next / 4U] = words[i];
    next += 4U;
    if (next >= maximum)
      next = minimum;
  }
  __asm__ volatile("" ::: "memory");
  svga_fifo[SVGA_FIFO_NEXT_CMD] = next;
  svga_write(SVGA_REG_SYNC, 1);
  return 0;
}

static int set_mode(u32 width, u32 height, struct vbox_video_mode *mode) {
  u32 max_width = svga_read(SVGA_REG_MAX_WIDTH);
  u32 max_height = svga_read(SVGA_REG_MAX_HEIGHT);
  if (width == 0 || height == 0 || width > max_width || height > max_height)
    return -1;

  svga_write(SVGA_REG_WIDTH, width);
  svga_write(SVGA_REG_HEIGHT, height);
  svga_write(SVGA_REG_BITS_PER_PIXEL, 32);
  svga_write(SVGA_REG_ENABLE, 1);

  u32 actual_width = svga_read(SVGA_REG_WIDTH);
  u32 actual_height = svga_read(SVGA_REG_HEIGHT);
  u32 pitch = svga_read(SVGA_REG_BYTES_PER_LINE);
  u32 bpp = svga_read(SVGA_REG_BITS_PER_PIXEL);
  u32 depth = svga_read(SVGA_REG_DEPTH);
  u32 pseudocolor = svga_read(SVGA_REG_PSEUDOCOLOR);
  u32 offset = svga_read(SVGA_REG_FB_OFFSET);
  if (actual_width != width || actual_height != height || bpp != 32 ||
      depth < 24 || pseudocolor || pitch < width * 4U ||
      offset >= svga_device.bar[1].size ||
      (u64)pitch * height > svga_device.bar[1].size - offset)
    return -1;

  if (mode) {
    *mode = (struct vbox_video_mode){
        .physical = svga_device.bar[1].base + offset,
        .width = actual_width,
        .height = actual_height,
        .pitch = pitch,
        .bpp = bpp,
        .red_mask = svga_read(SVGA_REG_RED_MASK),
        .green_mask = svga_read(SVGA_REG_GREEN_MASK),
        .blue_mask = svga_read(SVGA_REG_BLUE_MASK),
    };
  }
  return 0;
}

int vbox_video_get_mode(struct vbox_video_mode *mode) {
  if (!driver_active || !mode)
    return -1;
  u32 width = svga_read(SVGA_REG_WIDTH);
  u32 height = svga_read(SVGA_REG_HEIGHT);
  return set_mode(width, height, mode);
}

int vbox_video_init(u32 width, u32 height) {
  if (!pci_find_by_id(PCI_VENDOR_VMWARE, PCI_DEVICE_VMWARE_SVGA2,
                      &svga_device))
    return -1;

  struct pci_device vmmdev;
  if (!pci_find_by_id(PCI_VENDOR_VBOX, PCI_DEVICE_VBOX_VMMDEV, &vmmdev) ||
      !vmmdev.bar[0].valid || !vmmdev.bar[0].is_io ||
      vmmdev.bar[0].base > UINT16_MAX ||
      !svga_device.bar[0].valid || !svga_device.bar[0].is_io ||
      svga_device.bar[0].base > UINT16_MAX ||
      !svga_device.bar[1].valid || svga_device.bar[1].is_io ||
      !svga_device.bar[2].valid || svga_device.bar[2].is_io)
    return -1;

  pci_enable(&vmmdev);
  pci_enable(&svga_device);
  vmmdev_port = (u16)vmmdev.bar[0].base;
  svga_io_base = (u16)svga_device.bar[0].base;

  vmmdev_request_phys = pmm_alloc_frame_below(UINT32_MAX);
  if (!vmmdev_request_phys)
    return -1;
  vmmdev_request = phys_to_virt(vmmdev_request_phys);

  svga_write(SVGA_REG_ID, SVGA_ID_2);
  if (svga_read(SVGA_REG_ID) != SVGA_ID_2)
    goto fail;

  u32 reported_fifo_bytes = svga_read(SVGA_REG_MEM_SIZE);
  u64 fifo_limit = svga_device.bar[2].size;
  if (reported_fifo_bytes < fifo_limit)
    fifo_limit = reported_fifo_bytes;
  if (fifo_limit > UINT32_MAX)
    fifo_limit = UINT32_MAX;
  svga_fifo_bytes = (u32)fifo_limit;
  if (map_fifo(svga_device.bar[2].base, svga_fifo_bytes) != 0)
    goto fail;

  svga_write(SVGA_REG_CONFIG_DONE, 0);
  svga_fifo[SVGA_FIFO_MIN] = SVGA_FIFO_REGISTER_BYTES;
  svga_fifo[SVGA_FIFO_MAX] = svga_fifo_bytes;
  svga_fifo[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_REGISTER_BYTES;
  svga_fifo[SVGA_FIFO_STOP] = SVGA_FIFO_REGISTER_BYTES;
  __asm__ volatile("" ::: "memory");
  svga_write(SVGA_REG_CONFIG_DONE, 1);

  struct vmmdev_guest_info *info = vmmdev_request;
  request_init(&info->header, sizeof(*info),
               VMMDEV_REQUEST_REPORT_GUEST_INFO);
  info->interface_version = VMMDEV_INTERFACE_VERSION;
  info->os_type = VMMDEV_OSTYPE_LINUX26_X64;
  if (vmmdev_perform(&info->header) != 0)
    log_write("VBOX: guest-info report rejected; trying resize capability",
              KERNEL, LOG_WARN);

  struct vmmdev_guest_caps *caps = vmmdev_request;
  request_init(&caps->header, sizeof(*caps), VMMDEV_REQUEST_REPORT_GUEST_CAPS);
  caps->caps = VMMDEV_GUEST_SUPPORTS_GRAPHICS;
  if (vmmdev_perform(&caps->header) != 0)
    goto fail_fifo;

  struct vbox_video_mode mode;
  if (set_mode(width, height, &mode) != 0)
    goto fail_fifo;

  driver_active = 1;
  log_write("VBOX: VMMDev display hints + VMSVGA enabled", KERNEL, LOG_INFO);
  log_write_hex("VBOX: scanout physical =", mode.physical, KERNEL, LOG_INFO);
  log_write_hex("VBOX: FIFO bytes =", svga_fifo_bytes, KERNEL, LOG_INFO);
  return 0;

fail_fifo:
  svga_write(SVGA_REG_CONFIG_DONE, 0);
  unmap_fifo();
fail:
  pmm_free_frame(vmmdev_request_phys);
  vmmdev_request_phys = 0;
  vmmdev_request = 0;
  return -1;
}

int vbox_video_poll_resize(struct vbox_video_mode *mode) {
  if (!driver_active || !mode)
    return -1;

  struct vmmdev_display_change2 *request = vmmdev_request;
  request_init(&request->header, sizeof(*request),
               VMMDEV_REQUEST_GET_DISPLAY_CHANGE2);
  request->event_ack = VMMDEV_EVENT_DISPLAY_CHANGE;
  if (vmmdev_perform(&request->header) != 0)
    return -1;
  if (request->display != 0 || request->xres == 0 || request->yres == 0)
    return 0;

  u32 current_width = svga_read(SVGA_REG_WIDTH);
  u32 current_height = svga_read(SVGA_REG_HEIGHT);
  if (request->xres == current_width && request->yres == current_height)
    return 0;
  if (request->bpp != 0 && request->bpp != 32)
    return 0;

  if (set_mode(request->xres, request->yres, mode) != 0) {
    log_write("VBOX: host resize mode rejected by VMSVGA", KERNEL, LOG_WARN);
    return -1;
  }
  return 1;
}

int vbox_video_flush_rect(u32 x, u32 y, u32 width, u32 height) {
  if (!driver_active || width == 0 || height == 0)
    return -1;
  const u32 command[5] = {SVGA_CMD_UPDATE, x, y, width, height};
  return fifo_submit(command, 5);
}
