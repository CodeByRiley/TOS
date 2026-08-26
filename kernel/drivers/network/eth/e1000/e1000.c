#include <drivers/driver.h>
#include <drivers/network/eth/e1000/e1000.h>
#include <drivers/base/vendors/pci_ids.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <net/arp.h>
#include <net/eth.h>
#include <net/netif.h>
#include <net/netmon.h>
#include <pci/pci.h>
#include <utilities/log.h>
#include <utilities/string.h>
#include <utilities/types.h>

#define MMIO_VIRT_BASE 0xFFFFC00000000000ULL
#define E1000_MMIO_SIZE 0x20000ULL
#define E1000_POLL_LIMIT 1000000U
#define E1000_FRAME_MAX 1514U
#define E1000_FRAME_MIN 60U

/* QEMU's slirp gateway is 10.0.2.2 and it serves a /24. Static addressing
 * until DHCP lands, at which point netif_set_ipv4 replaces all three. */
static const uint8_t e1000_ipv4_addr[IPV4_ALEN] = {10, 0, 2, 30};
static const uint8_t e1000_ipv4_mask[IPV4_ALEN] = {255, 255, 255, 0};
static const uint8_t e1000_ipv4_gateway[IPV4_ALEN] = {10, 0, 2, 2};
static uint64_t next_mmio_virt = MMIO_VIRT_BASE;

static int e1000_wait_clear(struct e1000_dev *nic, uint32_t reg,
                            uint32_t mask) {
  for (uint32_t i = 0; i < E1000_POLL_LIMIT; i++) {
    if ((E1000_READ(nic, reg) & mask) == 0)
      return 0;
  }
  return -1;
}


static int e1000_eeprom_read(struct e1000_dev *nic, uint8_t word_offset,
                             uint16_t *out) {
  uint32_t eerd = ((uint32_t)word_offset << 8) | EERD_START;
  E1000_WRITE(nic, REG_EERD, eerd);

  for (uint32_t i = 0; i < E1000_POLL_LIMIT; i++) {
    uint32_t value = E1000_READ(nic, REG_EERD);
    if (value & EERD_DONE) {
      *out = (uint16_t)((value >> 16) & 0xFFFF);
      return 0;
    }
  }

  return -1;
}

static void e1000_read_mac_from_registers(struct e1000_dev *nic) {
  uint32_t ral = E1000_READ(nic, REG_RAL);
  uint32_t rah = E1000_READ(nic, REG_RAH);
  nic->mac_addr[0] = (uint8_t)(ral & 0xFF);
  nic->mac_addr[1] = (uint8_t)((ral >> 8) & 0xFF);
  nic->mac_addr[2] = (uint8_t)((ral >> 16) & 0xFF);
  nic->mac_addr[3] = (uint8_t)((ral >> 24) & 0xFF);
  nic->mac_addr[4] = (uint8_t)(rah & 0xFF);
  nic->mac_addr[5] = (uint8_t)((rah >> 8) & 0xFF);
}

static int e1000_read_mac_address(struct e1000_dev *nic) {
  uint16_t word0, word1, word2;

  if (e1000_eeprom_read(nic, 0, &word0) == 0 &&
      e1000_eeprom_read(nic, 1, &word1) == 0 &&
      e1000_eeprom_read(nic, 2, &word2) == 0) {
    nic->mac_addr[0] = (uint8_t)(word0 & 0xFF);
    nic->mac_addr[1] = (uint8_t)(word0 >> 8);
    nic->mac_addr[2] = (uint8_t)(word1 & 0xFF);
    nic->mac_addr[3] = (uint8_t)(word1 >> 8);
    nic->mac_addr[4] = (uint8_t)(word2 & 0xFF);
    nic->mac_addr[5] = (uint8_t)(word2 >> 8);
  } else {
    log_write("e1000: EEPROM read timed out, using receive address", KERNEL,
              LOG_WARN);
    e1000_read_mac_from_registers(nic);
  }

  uint32_t mac_low = nic->mac_addr[0] | (nic->mac_addr[1] << 8) |
                     (nic->mac_addr[2] << 16) | (nic->mac_addr[3] << 24);
  uint32_t mac_high = nic->mac_addr[4] | (nic->mac_addr[5] << 8);

  E1000_WRITE(nic, 0x5400, mac_low);
  E1000_WRITE(nic, 0x5404, mac_high | (1 << 31)); // Set AV bit

  log_write_fmt(KERNEL, LOG_INFO, "MAC Address: %02x:%02x:%02x:%02x:%02x:%02x",
                nic->mac_addr[0], nic->mac_addr[1], nic->mac_addr[2],
                nic->mac_addr[3], nic->mac_addr[4], nic->mac_addr[5]);
  return 0;
}

static int e1000_alloc_rx_ring(struct e1000_dev *nic) {
  uint64_t phys = pmm_alloc_frame();
  if (!phys)
    return -1;

  nic->rx_ring_phys = phys;
  nic->rx_ring_virt = (struct e1000_rx_desc *)phys_to_virt(phys);
  memset(nic->rx_ring_virt, 0, FRAME_SIZE);
  return 0;
}

static int e1000_alloc_rx_buffers(struct e1000_dev *nic) {
  nic->rx_current = 0;
  for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys)
      return -1;

    nic->rx_buffer_phys[i] = phys;
    nic->rx_buffers[i] = phys_to_virt(phys);
    memset(nic->rx_buffers[i], 0, FRAME_SIZE);
    nic->rx_ring_virt[i].addr = phys;
    nic->rx_ring_virt[i].status = 0;
  }
  return 0;
}

static void e1000_setup_rx_registers(struct e1000_dev *nic) {
  E1000_WRITE(nic, REG_RCTRL, 0);
  E1000_WRITE(nic, REG_RXDESCLO, (u32)(nic->rx_ring_phys & 0xFFFFFFFF));
  E1000_WRITE(nic, REG_RXDESCHI, (u32)((nic->rx_ring_phys >> 32) & 0xFFFFFFFF));
  E1000_WRITE(nic, REG_RXDESCLEN,
              E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc));
  E1000_WRITE(nic, REG_RXDESCHEAD, 0);
  E1000_WRITE(nic, REG_RXDESCTAIL, E1000_NUM_RX_DESC - 1);

  u32 rctl = RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC;
  rctl |= RCTL_UPE;
  rctl |= RCTL_MPE;
  E1000_WRITE(nic, REG_RCTRL, rctl);
}

static int e1000_alloc_tx_ring(struct e1000_dev *nic) {
  uint64_t phys = pmm_alloc_frame();
  if (!phys)
    return -1;

  nic->tx_ring_phys = phys;
  nic->tx_ring_virt = (struct e1000_tx_desc *)phys_to_virt(phys);
  memset(nic->tx_ring_virt, 0, FRAME_SIZE);
  nic->tx_current = 0;

  for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
    uint64_t buf_phys = pmm_alloc_frame();
    if (!buf_phys)
      return -1;

    nic->tx_buffer_phys[i] = buf_phys;
    nic->tx_buffers[i] = phys_to_virt(buf_phys);
    memset(nic->tx_buffers[i], 0, FRAME_SIZE);
    nic->tx_ring_virt[i].addr = buf_phys;
    nic->tx_ring_virt[i].status = TSTA_DD;
  }

  return 0;
}

static void e1000_setup_tx_registers(struct e1000_dev *nic) {
  E1000_WRITE(nic, REG_TCTRL, 0);
  E1000_WRITE(nic, REG_TXDESCLO, (u32)(nic->tx_ring_phys & 0xFFFFFFFF));
  E1000_WRITE(nic, REG_TXDESCHI, (u32)((nic->tx_ring_phys >> 32) & 0xFFFFFFFF));
  E1000_WRITE(nic, REG_TXDESCLEN,
              E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc));
  E1000_WRITE(nic, REG_TXDESCHEAD, 0);
  E1000_WRITE(nic, REG_TXDESCTAIL, 0);

  E1000_WRITE(nic, REG_TIPG, 10 | (8 << 10) | (6 << 20));

  u32 tctl =
      TCTL_EN | TCTL_PSP | (0x10 << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT);
  E1000_WRITE(nic, REG_TCTRL, tctl);
}

static void e1000_enable_bus_master(const struct pci_device *pci) {
  uint16_t cmd = pci_read16(pci->addr, PCI_CFG_COMMAND);
  cmd |= PCI_CMD_MEM | PCI_CMD_BUS_MASTER;
  pci_write16(pci->addr, PCI_CFG_COMMAND, cmd);
}

/* netif tx callback. Must not block: reached from the driver poll task
 * today and from the IRQ handler once that path lands. A full ring is
 * reported as failure rather than spun on. */
static int e1000_tx(void *driver_data, const void *frame, uint16_t len) {
  struct e1000_dev *nic = (struct e1000_dev *)driver_data;
  if (!nic || !frame || len > E1000_FRAME_MAX)
    return -1;

  uint32_t idx = nic->tx_current;
  struct e1000_tx_desc *desc = &nic->tx_ring_virt[idx];
  if (!(desc->status & TSTA_DD)) {
    log_write("e1000: TX ring full! Cannot send reply", KERNEL, LOG_ERROR);
    return -1;
  }
  log_write("e1000: Forwarding TX packet!", KERNEL, LOG_INFO);
  uint16_t wire_len = len < E1000_FRAME_MIN ? E1000_FRAME_MIN : len;
  memcpy(nic->tx_buffers[idx], frame, len);
  if (wire_len > len) {
    memset((uint8_t *)nic->tx_buffers[idx] + len, 0, wire_len - len);
  }

  desc->addr = nic->tx_buffer_phys[idx];
  desc->len = wire_len;
  desc->cso = 0;
  desc->cmd = CMD_EOP | CMD_IFCS | CMD_RS;
  desc->status = 0;
  desc->css = 0;
  desc->special = 0;

  nic->tx_current = (idx + 1) % E1000_NUM_TX_DESC;
  E1000_WRITE(nic, REG_TXDESCTAIL, nic->tx_current);
  netmon_record(NETMON_DIR_TX, frame, len);
  return 0;
}

static int e1000_match(const struct device *dev) {
  if (dev->bus != DEVICE_BUS_PCI)
    return 0;

  return dev->bus_info.pci.vendor == PCI_VENDOR_INTEL &&
         dev->bus_info.pci.device == PCI_DEVICE_INTEL_E1000;
}

static int e1000_init_hardware(struct e1000_dev *nic) {
  E1000_WRITE(nic, REG_IMC, 0xFFFFFFFF);
  (void)E1000_READ(nic, REG_ICR);

  u32 ctrl = E1000_READ(nic, REG_CTRL);
  E1000_WRITE(nic, REG_CTRL, ctrl | ECTRL_RST);
  if (e1000_wait_clear(nic, REG_CTRL, ECTRL_RST) != 0) {
    log_write("e1000: reset timed out", KERNEL, LOG_ERROR);
    return -1;
  }

  E1000_WRITE(nic, REG_IMS, 0x01); // Unmask ICR_RXDMT0 (Receive Interrupt)
  (void)E1000_READ(nic, REG_ICR);

  ctrl = E1000_READ(nic, REG_CTRL);
  ctrl |= ECTRL_SLU | ECTRL_FD;
  E1000_WRITE(nic, REG_CTRL, ctrl);

  for (int i = 0; i < 128; i++) {
    E1000_WRITE(nic, 0x5200 + (i * 4), 0);
  }

  log_write("[e1000] Device reset and link set up.", KERNEL, LOG_INFO);

  if (e1000_read_mac_address(nic) != 0)
    return -1;

  if (e1000_alloc_rx_ring(nic) != 0 || e1000_alloc_rx_buffers(nic) != 0) {
    log_write("e1000: RX allocation failed", KERNEL, LOG_ERROR);
    return -1;
  }
  if (e1000_alloc_tx_ring(nic) != 0) {
    log_write("e1000: TX allocation failed", KERNEL, LOG_ERROR);
    return -1;
  }

  e1000_setup_tx_registers(nic);
  e1000_setup_rx_registers(nic);

  netmon_bind(nic->mac_addr, e1000_ipv4_addr);

  /* Publish the interface only now: netif_register makes the whole stack
   * reachable, and anything that transmits before the rings are set up
   * writes into descriptors the hardware has not been told about. */
  struct netif nif;
  memset(&nif, 0, sizeof(nif));
  memcpy(nif.mac, nic->mac_addr, NETIF_MAC_LEN);
  memcpy(nif.ipv4, e1000_ipv4_addr, IPV4_ALEN);
  memcpy(nif.netmask, e1000_ipv4_mask, IPV4_ALEN);
  memcpy(nif.gateway, e1000_ipv4_gateway, IPV4_ALEN);
  nif.mtu = E1000_FRAME_MAX - ETH_HDR_LEN;
  nif.driver_data = nic;
  nif.tx = e1000_tx;
  netif_register(&nif);

  /* Announce the binding now that transmit is live, and resolve the
   * gateway while nothing is waiting on it. */
  arp_announce();
  arp_prime_gateway();

  log_write("e1000: RX/TX ready at 10.0.2.30", KERNEL, LOG_INFO);

  //e1000_send_test_ping(nic);

  return 0;
}

/* STATUS bit 1 is Link Up; bits 7:6 encode 10 / 100 / 1000 Mb/s. */
static void e1000_report_link(struct e1000_dev *nic) {
  uint32_t status = E1000_READ(nic, REG_STATUS);
  static const uint32_t speeds[4] = {10, 100, 1000, 1000};
  netmon_set_link((status & 0x2u) != 0, speeds[(status >> 6) & 0x3u]);
}

static int e1000_poll_rx(struct device *dev) {
  struct e1000_dev *nic = (struct e1000_dev *)dev->driver_data;
  if (!nic)
    return 0;

  /* Link state and ARP timers run every pass and are not "work": counting
   * them would keep the poll task spinning on a completely idle link. */
  e1000_report_link(nic);

  int handled = 0;

  for (int budget = 0; budget < E1000_NUM_RX_DESC; budget++) {
    struct e1000_rx_desc *desc = &nic->rx_ring_virt[nic->rx_current];

    // If the Descriptor Done (DD) bit is not set, no more packets
    if (!(desc->status & 0x01))
      break;

    // We have a valid packet!
    uint16_t packet_len = desc->len;

    /* tests/netmon_test.py counts this line to check the RX ring keeps
     * receiving past the first frame. One line per frame, not a flood -
     * silence it and that test reports a wedged ring that is fine. */
    log_write_fmt(KERNEL, LOG_INFO, "[e1000] Received packet! Length: %d bytes\n", packet_len);
    // log_write_fmt(KERNEL, LOG_INFO, "        rx_current: %d, rx_ring_phys: %p, rx_buffers[rx_current]: %p\n", nic->rx_current, nic->rx_ring_phys, nic->rx_buffers[nic->rx_current]);

    netmon_record(NETMON_DIR_RX, nic->rx_buffers[nic->rx_current],
                  packet_len);

    // Hand the frame to the stack. netmon above saw it either way; the
    // address filter and protocol demux live in eth_input now.
    log_write_fmt(KERNEL, LOG_INFO, "[e1000] Packet length: %d expecting 1514", packet_len);
    if (packet_len <= E1000_FRAME_MAX) {
      log_write_fmt(KERNEL, LOG_INFO, "[e1000] Packet length is less than max, we accept", packet_len);
      eth_input((const uint8_t *)nic->rx_buffers[nic->rx_current], packet_len);
    } else {
      log_write_fmt(KERNEL, LOG_WARN, "[e1000] Dropping oversized frame of length %d", packet_len);
    }

    // Give the buffer back to the hardware.
    //
    // RDT names the descriptor the NIC must NOT write, so the hardware
    // owns [RDH, RDT) and software owns the rest. Releasing a descriptor
    // therefore means pointing RDT *at* the one just consumed, leaving it
    // one behind rx_current. Writing rx_current itself -- which is what
    // this did -- collapses RDH and RDT onto the same index after the
    // very first frame, so the NIC decides it has nowhere to put anything
    // and silently drops every packet from then on.
    desc->status = 0; // Clear DD bit
    uint32_t consumed = nic->rx_current;
    nic->rx_current = (nic->rx_current + 1) % E1000_NUM_RX_DESC;
    E1000_WRITE(nic, REG_RXDESCTAIL, consumed);
    handled++;
  }

  /* ARP timeouts ride the poll task instead of earning a timer of their
   * own; the cache is empty and this returns immediately on most passes. */
  arp_tick();

  return handled;
}

static int e1000_probe(struct device *dev) {
  struct e1000_dev *nic = kmalloc(sizeof(struct e1000_dev));
  if (!nic)
    return -1;
  memset(nic, 0, sizeof(*nic));

  const struct pci_bar *bar0 = &dev->bus_info.pci.bar[0];
  if (!bar0->valid || bar0->is_io || bar0->size < E1000_MMIO_SIZE) {
    log_write("e1000: invalid BAR0", KERNEL, LOG_ERROR);
    kfree(nic);
    return -1;
  }

  if (pci_enable_memory(&dev->bus_info.pci) != 0) {
    log_write("e1000: could not enable MMIO decoding", KERNEL, LOG_ERROR);
    kfree(nic);
    return -1;
  }

  u64 mmio_virt = next_mmio_virt;
  next_mmio_virt += 0x20000;

  u64 flags = VMM_PRESENT | VMM_WRITE | VMM_PCD | VMM_PWT;
  for (u64 i = 0; i < 0x20000; i += 0x1000) {
    if (vmm_map(mmio_virt + i, bar0->base + i, flags) != 0) {
      log_write("e1000: MMIO map failed", KERNEL, LOG_ERROR);
      kfree(nic);
      return -1;
    }
  }

  nic->mmio_base = (volatile uint8_t *)mmio_virt;
  nic->bus = dev->bus_info.pci.addr.bus;
  nic->device = dev->bus_info.pci.addr.dev;
  nic->function = dev->bus_info.pci.addr.fn;

  dev->driver_data = nic;
  e1000_enable_bus_master(&dev->bus_info.pci);

  if (e1000_init_hardware(nic) != 0) {
    dev->driver_data = 0;
    kfree(nic);
    return -1;
  }

  return 0;
}

static struct driver e1000_driver = {.name = "e1000 Ethernet",
                                     .bus = DEVICE_BUS_PCI,
                                     .match = e1000_match,
                                     .probe = e1000_probe,
                                     .poll = e1000_poll_rx};

void e1000_driver_init(void) {
  log_write("e1000: initialising", KERNEL, LOG_INFO);
  driver_register(&e1000_driver);
}
