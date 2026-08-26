#ifndef E1000_H
#define E1000_H


/* PACKED and friends. */
#include <utilities/types.h>
#include <drivers/driver.h>
#include <drivers/base/vendors/pci_ids.h>

#ifndef PCI_VENDOR_INTEL
#define PCI_VENDOR_INTEL 0x8086
#endif

#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EECD 0x0010
#define REG_EERD 0x0014
#define REG_CTRL_EXT 0x0018
#define REG_RAL 0x5400
#define REG_RAH 0x5404

// Interrupt Cause Read
#define REG_ICR 0x000C0
// Interrupt Throttling
#define REG_ITR 0x000C4
// Interrupt Cause Set
#define REG_ICS 0x00C8
// Interrupt Mask Set/Read
#define REG_IMS 0x00D4
// Interrupt Mask Clear
#define REG_IMC 0x00D8

#define REG_IMASK 0x00D0
#define REG_RCTRL 0x0100
#define REG_RXDESCLO 0x2800
#define REG_RXDESCHI 0x2804
#define REG_RXDESCLEN 0x2808
#define REG_RXDESCHEAD 0x2810
#define REG_RXDESCTAIL 0x2818

#define REG_TCTRL 0x0400
#define REG_TXDESCLO 0x3800
#define REG_TXDESCHI 0x3804
#define REG_TXDESCLEN 0x3808
#define REG_TXDESCHEAD 0x3810
#define REG_TXDESCTAIL 0x3818

#define REG_RDTR 0x2820   // RX Delay Timer Register
#define REG_RXDCTL 0x2828 // RX Descriptor Control
#define REG_RADV 0x282C   // RX Int. Absolute Delay Timer
#define REG_RSRPD 0x2C00  // RX Small Packet Detect Interrupt

#define REG_TIPG 0x0410 // Transmit Inter Packet Gap
#define ECTRL_FD (1 << 0)
#define ECTRL_SLU 0x40  // set link up
#define ECTRL_RST (1 << 26)

#define EERD_START (1 << 0)
#define EERD_DONE  (1 << 4)

#define RCTL_EN (1 << 1)            // Receiver Enable
#define RCTL_SBP (1 << 2)           // Store Bad Packets
#define RCTL_UPE (1 << 3)           // Unicast Promiscuous Enabled
#define RCTL_MPE (1 << 4)           // Multicast Promiscuous Enabled
#define RCTL_LPE (1 << 5)           // Long Packet Reception Enable
#define RCTL_LBM_NONE (0 << 6)      // No Loopback
#define RCTL_LBM_PHY (3 << 6)       // PHY or external SerDesc loopback
#define RTCL_RDMTS_HALF (0 << 8)    // Free Buffer Threshold is 1/2 of RDLEN
#define RTCL_RDMTS_QUARTER (1 << 8) // Free Buffer Threshold is 1/4 of RDLEN
#define RTCL_RDMTS_EIGHTH (2 << 8)  // Free Buffer Threshold is 1/8 of RDLEN
#define RCTL_MO_36 (0 << 12)        // Multicast Offset - bits 47:36
#define RCTL_MO_35 (1 << 12)        // Multicast Offset - bits 46:35
#define RCTL_MO_34 (2 << 12)        // Multicast Offset - bits 45:34
#define RCTL_MO_32 (3 << 12)        // Multicast Offset - bits 43:32
#define RCTL_BAM (1 << 15)          // Broadcast Accept Mode
#define RCTL_VFE (1 << 18)          // VLAN Filter Enable
#define RCTL_CFIEN (1 << 19)        // Canonical Form Indicator Enable
#define RCTL_CFI (1 << 20)          // Canonical Form Indicator Bit Value
#define RCTL_DPF (1 << 22)          // Discard Pause Frames
#define RCTL_PMCF (1 << 23)         // Pass MAC Control Frames
#define RCTL_SECRC (1 << 26)        // Strip Ethernet CRC

// Buffer Sizes
#define RCTL_BSIZE_256 (3 << 16)
#define RCTL_BSIZE_512 (2 << 16)
#define RCTL_BSIZE_1024 (1 << 16)
#define RCTL_BSIZE_2048 (0 << 16)
#define RCTL_BSIZE_4096 ((3 << 16) | (1 << 25))
#define RCTL_BSIZE_8192 ((2 << 16) | (1 << 25))
#define RCTL_BSIZE_16384 ((1 << 16) | (1 << 25))

// Transmit Command

#define CMD_EOP (1 << 0)  // End of Packet
#define CMD_IFCS (1 << 1) // Insert FCS
#define CMD_IC (1 << 2)   // Insert Checksum
#define CMD_RS (1 << 3)   // Report Status
#define CMD_RPS (1 << 4)  // Report Packet Sent
#define CMD_VLE (1 << 6)  // VLAN Packet Enable
#define CMD_IDE (1 << 7)  // Interrupt Delay Enable

// TCTL Register

#define TCTL_EN (1 << 1)      // Transmit Enable
#define TCTL_PSP (1 << 3)     // Pad Short Packets
#define TCTL_CT_SHIFT 4       // Collision Threshold
#define TCTL_COLD_SHIFT 12    // Collision Distance
#define TCTL_SWXOFF (1 << 22) // Software XOFF Transmission
#define TCTL_RTLC (1 << 24)   // Re-transmit on Late Collision

#define TSTA_DD (1 << 0) // Descriptor Done
#define TSTA_EC (1 << 1) // Excess Collisions
#define TSTA_LC (1 << 2) // Late Collision
#define LSTA_TU (1 << 3) // Transmit Underrun

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8

#define E1000_READ(nic, reg)                                                   \
  (*((volatile uint32_t *)((nic)->mmio_base + (reg))))
#define E1000_WRITE(nic, reg, val)                                             \
  (*((volatile uint32_t *)((nic)->mmio_base + (reg))) = (val))

#include <stdint.h>

struct e1000_dev {
  uint16_t bus;
  uint16_t device;
  uint16_t function;

  volatile uint8_t *mmio_base;

  struct e1000_rx_desc *rx_ring_virt;
  uintptr_t rx_ring_phys;             // Programmed into REG_RXDESCLO.
  uintptr_t rx_buffer_phys[E1000_NUM_RX_DESC];
  void *rx_buffers[E1000_NUM_RX_DESC];
  uint32_t rx_current;

  struct e1000_tx_desc *tx_ring_virt;
  uintptr_t tx_ring_phys;
  uintptr_t tx_buffer_phys[E1000_NUM_TX_DESC];
  void *tx_buffers[E1000_NUM_TX_DESC];
  uint32_t tx_current;

  uint8_t mac_addr[6];
};

struct e1000_rx_desc {
  volatile uint64_t addr;
  volatile uint16_t len;
  volatile uint16_t checksum;
  volatile uint8_t status;
  volatile uint8_t errors;
  volatile uint16_t special;
} PACKED;

struct e1000_tx_desc {
  volatile uint64_t addr;      // 0: Buffer Address
  volatile uint16_t len;       // 8: Data Length
  volatile uint8_t  cso;       // 10: Checksum Offset (usually 0)
  volatile uint8_t  cmd;       // 11: Command (EOP, IFCS, RS go here)
  volatile uint8_t  status;    // 12: Status (DD bit goes here)
  volatile uint8_t  css;       // 13: Checksum Start (usually 0)
  volatile uint16_t special;   // 14: Special / VLAN tag (usually 0)
} PACKED;

void e1000_driver_init(void);

#endif // E1000_H
