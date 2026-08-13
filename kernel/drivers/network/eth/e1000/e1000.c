#include <drivers/driver.h>
#include <drivers/network/eth/e1000/e1000.h>
#include <drivers/base/vendors/pci_ids.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <pci/pci.h>
#include <utilities/log.h>
#include <utilities/string.h>
#include <utilities/types.h>

#define MMIO_VIRT_BASE 0xFFFFC00000000000ULL
#define E1000_MMIO_SIZE 0x20000ULL
#define E1000_POLL_LIMIT 1000000U
#define E1000_FRAME_MAX 1514U
#define E1000_FRAME_MIN 60U

#define ETH_TYPE_IPV4 0x0800U
#define ETH_TYPE_ARP 0x0806U
#define ARP_OP_REQUEST 1U
#define ARP_OP_REPLY 2U
#define IPPROTO_ICMP 1U
#define ICMP_ECHO_REPLY 0U
#define ICMP_ECHO_REQUEST 8U

static const uint8_t e1000_ipv4_addr[4] = {10, 0, 2, 30};
static uint64_t next_mmio_virt = MMIO_VIRT_BASE;

struct eth_hdr {
  uint8_t dst[6];
  uint8_t src[6];
  uint16_t type;
} __attribute__((packed));

struct arp_ipv4 {
  uint16_t htype;
  uint16_t ptype;
  uint8_t hlen;
  uint8_t plen;
  uint16_t oper;
  uint8_t sha[6];
  uint8_t spa[4];
  uint8_t tha[6];
  uint8_t tpa[4];
} __attribute__((packed));

struct ipv4_hdr {
  uint8_t version_ihl;
  uint8_t tos;
  uint16_t total_length;
  uint16_t id;
  uint16_t flags_fragment;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t checksum;
  uint8_t src[4];
  uint8_t dst[4];
} __attribute__((packed));

struct icmp_hdr {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t ident;
  uint16_t sequence;
} __attribute__((packed));

static uint16_t bswap16(uint16_t value) {
  return (uint16_t)((value << 8) | (value >> 8));
}

static uint16_t from_be16(uint16_t value) { return bswap16(value); }
static uint16_t to_be16(uint16_t value) { return bswap16(value); }

static uint16_t inet_checksum(const void *data, uint32_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t sum = 0;

  while (len > 1) {
    sum += ((uint16_t)bytes[0] << 8) | bytes[1];
    bytes += 2;
    len -= 2;
  }
  if (len)
    sum += (uint16_t)bytes[0] << 8;

  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);

  return (uint16_t)~sum;
}

static int ipv4_addr_equal(const uint8_t *a, const uint8_t *b) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

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

  // Write MAC to Receive Address Register 0
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
  cmd |= PCI_CMD_MEM | PCI_CMD_BUS_MASTER | PCI_CMD_INT_DISABLE;
  pci_write16(pci->addr, PCI_CFG_COMMAND, cmd);
}

static int e1000_send_frame(struct e1000_dev *nic, const void *frame,
                            uint16_t len) {
  if (!nic || !frame || len > E1000_FRAME_MAX)
    return -1;

  uint32_t idx = nic->tx_current;
  struct e1000_tx_desc *desc = &nic->tx_ring_virt[idx];
  if (!(desc->status & TSTA_DD))
    return -1;

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
  return 0;
}

// void e1000_send_test_ping(struct e1000_dev *nic) {
//     uint8_t frame[74]; // 14 (eth) + 20 (ip) + 8 (icmp) + 32 (data)
//     memset(frame, 0, sizeof(frame));

//     struct eth_hdr *eth = (struct eth_hdr *)frame;
//     // QEMU's gateway MAC address is always 52:55:0a:00:02:02
//     uint8_t gw_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
//     memcpy(eth->dst, gw_mac, 6);
//     memcpy(eth->src, nic->mac_addr, 6);
//     eth->type = to_be16(ETH_TYPE_IPV4);

//     struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + 14);
//     ip->version_ihl = 0x45;
//     ip->tos = 0;
//     ip->total_length = to_be16(20 + 8 + 32); // IP + ICMP + Data
//     ip->id = to_be16(1);
//     ip->flags_fragment = 0;
//     ip->ttl = 64;
//     ip->protocol = IPPROTO_ICMP;
//     uint8_t src_ip[4] = {10, 0, 2, 30};
//     uint8_t dst_ip[4] = {10, 0, 2, 2}; // 10.0.2.2 is QEMU's gateway
//     memcpy(ip->src, src_ip, 4);
//     memcpy(ip->dst, dst_ip, 4);
//     ip->checksum = 0;
//     ip->checksum = to_be16(inet_checksum(ip, 20));

//     struct icmp_hdr *icmp = (struct icmp_hdr *)(frame + 14 + 20);
//     icmp->type = ICMP_ECHO_REQUEST;
//     icmp->code = 0;
//     icmp->ident = to_be16(1);
//     icmp->sequence = to_be16(1);
//     // ICMP data (32 bytes of 'A')
//     memset(frame + 14 + 20 + 8, 'A', 32);
//     icmp->checksum = 0;
//     icmp->checksum = to_be16(inet_checksum(icmp, 8 + 32));

//     e1000_send_frame(nic, frame, 74);
//     log_write("e1000: Sent test ICMP ping to 10.0.2.2", KERNEL, LOG_INFO);
// }

static void e1000_handle_arp(struct e1000_dev *nic, const struct eth_hdr *eth,
                             const uint8_t *payload, uint16_t len) {
  if (len < sizeof(struct arp_ipv4))
    return;

  const struct arp_ipv4 *arp = (const struct arp_ipv4 *)payload;
  if (from_be16(arp->htype) != 1 || from_be16(arp->ptype) != ETH_TYPE_IPV4 ||
      arp->hlen != 6 || arp->plen != 4 ||
      from_be16(arp->oper) != ARP_OP_REQUEST ||
      !ipv4_addr_equal(arp->tpa, e1000_ipv4_addr)) {
    return;
  }

  uint8_t frame[sizeof(struct eth_hdr) + sizeof(struct arp_ipv4)];
  struct eth_hdr *reply_eth = (struct eth_hdr *)frame;
  struct arp_ipv4 *reply_arp =
      (struct arp_ipv4 *)(frame + sizeof(struct eth_hdr));

  memcpy(reply_eth->dst, eth->src, sizeof(reply_eth->dst));
  memcpy(reply_eth->src, nic->mac_addr, sizeof(reply_eth->src));
  reply_eth->type = to_be16(ETH_TYPE_ARP);

  reply_arp->htype = to_be16(1);
  reply_arp->ptype = to_be16(ETH_TYPE_IPV4);
  reply_arp->hlen = 6;
  reply_arp->plen = 4;
  reply_arp->oper = to_be16(ARP_OP_REPLY);
  memcpy(reply_arp->sha, nic->mac_addr, sizeof(reply_arp->sha));
  memcpy(reply_arp->spa, e1000_ipv4_addr, sizeof(reply_arp->spa));
  memcpy(reply_arp->tha, arp->sha, sizeof(reply_arp->tha));
  memcpy(reply_arp->tpa, arp->spa, sizeof(reply_arp->tpa));

  e1000_send_frame(nic, frame, sizeof(frame));
}

static void e1000_handle_icmp(struct e1000_dev *nic, const struct eth_hdr *eth,
                              const uint8_t *packet, uint16_t len) {
  if (len < sizeof(struct ipv4_hdr))
    return;

  const struct ipv4_hdr *ip = (const struct ipv4_hdr *)packet;
  uint8_t ihl = (uint8_t)((ip->version_ihl & 0x0F) * 4);
  uint16_t total_len = from_be16(ip->total_length);

  if ((ip->version_ihl >> 4) != 4 || ihl < sizeof(struct ipv4_hdr) ||
      total_len < ihl + sizeof(struct icmp_hdr) || total_len > len ||
      ip->protocol != IPPROTO_ICMP ||
      !ipv4_addr_equal(ip->dst, e1000_ipv4_addr)) {
    return;
  }

  uint16_t frag = from_be16(ip->flags_fragment);
  if (frag & 0x3FFF)
    return;

  const struct icmp_hdr *icmp = (const struct icmp_hdr *)(packet + ihl);
  uint16_t icmp_len = total_len - ihl;
  if (icmp->type != ICMP_ECHO_REQUEST || icmp->code != 0)
    return;

  uint16_t frame_len = (uint16_t)(sizeof(struct eth_hdr) + total_len);
  if (frame_len > E1000_FRAME_MAX)
    return;

  uint8_t frame[E1000_FRAME_MAX];
  memcpy(frame, eth, frame_len);

  struct eth_hdr *reply_eth = (struct eth_hdr *)frame;
  struct ipv4_hdr *reply_ip =
      (struct ipv4_hdr *)(frame + sizeof(struct eth_hdr));
  struct icmp_hdr *reply_icmp = (struct icmp_hdr *)((uint8_t *)reply_ip + ihl);

  memcpy(reply_eth->dst, eth->src, sizeof(reply_eth->dst));
  memcpy(reply_eth->src, nic->mac_addr, sizeof(reply_eth->src));

  uint8_t tmp_ip[4];
  memcpy(tmp_ip, reply_ip->src, sizeof(tmp_ip));
  memcpy(reply_ip->src, e1000_ipv4_addr, sizeof(reply_ip->src));
  memcpy(reply_ip->dst, tmp_ip, sizeof(reply_ip->dst));
  reply_ip->ttl = 64;
  reply_ip->checksum = 0;
  reply_ip->checksum = to_be16(inet_checksum(reply_ip, ihl));

  reply_icmp->type = ICMP_ECHO_REPLY;
  reply_icmp->checksum = 0;
  reply_icmp->checksum = to_be16(inet_checksum(reply_icmp, icmp_len));

  e1000_send_frame(nic, frame, frame_len);
}

static void e1000_handle_frame(struct e1000_dev *nic, const uint8_t *packet,
                               uint16_t len) {
  if (len < sizeof(struct eth_hdr))
    return;

  const struct eth_hdr *eth = (const struct eth_hdr *)packet;
  uint16_t type = from_be16(eth->type);
  const uint8_t *payload = packet + sizeof(struct eth_hdr);
  uint16_t payload_len = (uint16_t)(len - sizeof(struct eth_hdr));
  log_write_hex("e1000: received frame of type", type, KERNEL, LOG_INFO);
  if (type == ETH_TYPE_ARP) {
    e1000_handle_arp(nic, eth, payload, payload_len);
  } else if (type == ETH_TYPE_IPV4) {
    e1000_handle_icmp(nic, eth, payload, payload_len);
  }
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

  E1000_WRITE(nic, REG_IMC, 0xFFFFFFFF);
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

  log_write("e1000: RX/TX ready at 10.0.2.30", KERNEL, LOG_INFO);

  //e1000_send_test_ping(nic);

  return 0;
}

static void e1000_poll_rx(struct device *dev) {
  struct e1000_dev *nic = (struct e1000_dev *)dev->driver_data;
  if (!nic)
    return;

  for (int budget = 0; budget < E1000_NUM_RX_DESC; budget++) {
    struct e1000_rx_desc *desc = &nic->rx_ring_virt[nic->rx_current];

    // If the Descriptor Done (DD) bit is not set, no more packets
    if (!(desc->status & 0x01))
      break;

    // We have a valid packet!
    uint16_t packet_len = desc->len;

    log_write_fmt(KERNEL, LOG_INFO, "[e1000] Received packet! Length: %d bytes\n", packet_len);

    // Pass the packet to the network stack (ARP/ICMP)
    if (packet_len <= E1000_FRAME_MAX) {
      e1000_handle_frame(nic, (const uint8_t *)nic->rx_buffers[nic->rx_current], packet_len);
    }

    // Give the buffer back to the hardware
    desc->status = 0; // Clear DD bit
    nic->rx_current = (nic->rx_current + 1) % E1000_NUM_RX_DESC;
    E1000_WRITE(nic, REG_RXDESCTAIL, nic->rx_current); // Tell hardware we are done
  }
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
