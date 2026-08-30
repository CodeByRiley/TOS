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
#define REG_ICR 0x00C0
// Interrupt Throttling
#define REG_ITR 0x00C4
// Interrupt Cause Set
#define REG_ICS 0x00C8
// Interrupt Mask Set/Read
#define REG_IMS 0x00D0
// Interrupt Mask Clear
#define REG_IMC 0x00D8

#define REG_IMASK REG_IMS // Interrupt Mask Set/Read alias
#define REG_RCTL 0x0100

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
#define CTRL_FD  (UINT32_C(1) << 0)
#define CTRL_SLU (UINT32_C(1) << 6)  // set link up
#define CTRL_RST (UINT32_C(1) << 26)

#define EERD_START (UINT32_C(1) << 0)
#define EERD_DONE  (UINT32_C(1) << 4)

#define RCTL_EN (UINT32_C(1) << 1)            // Receiver Enable
#define RCTL_SBP (UINT32_C(1) << 2)           // Store Bad Packets
#define RCTL_UPE (UINT32_C(1) << 3)           // Unicast Promiscuous Enabled
#define RCTL_MPE (UINT32_C(1) << 4)           // Multicast Promiscuous Enabled
#define RCTL_LPE (UINT32_C(1) << 5)           // Long Packet Reception Enable
#define RCTL_LBM_NONE (UINT32_C(0) << 6)      // No Loopback
#define RCTL_LBM_PHY (UINT32_C(3) << 6)       // PHY or external SerDesc loopback
#define RCTL_RDMTS_HALF (UINT32_C(0) << 8)    // Free Buffer Threshold is 1/2 of RDLEN
#define RCTL_RDMTS_QUARTER (UINT32_C(1) << 8) // Free Buffer Threshold is 1/4 of RDLEN
#define RCTL_RDMTS_EIGHTH (UINT32_C(2) << 8)  // Free Buffer Threshold is 1/8 of RDLEN
#define RCTL_MO_36 (UINT32_C(0) << 12)        // Multicast Offset - bits 47:36
#define RCTL_MO_35 (UINT32_C(1) << 12)        // Multicast Offset - bits 46:35
#define RCTL_MO_34 (UINT32_C(2) << 12)        // Multicast Offset - bits 45:34
#define RCTL_MO_32 (UINT32_C(3) << 12)        // Multicast Offset - bits 43:32
#define RCTL_BAM (UINT32_C(1) << 15)          // Broadcast Accept Mode
#define RCTL_VFE (UINT32_C(1) << 18)          // VLAN Filter Enable
#define RCTL_CFIEN (UINT32_C(1) << 19)        // Canonical Form Indicator Enable
#define RCTL_CFI (UINT32_C(1) << 20)          // Canonical Form Indicator Bit Value
#define RCTL_DPF (UINT32_C(1) << 22)          // Discard Pause Frames
#define RCTL_PMCF (UINT32_C(1) << 23)         // Pass MAC Control Frames
#define RCTL_SECRC (UINT32_C(1) << 26)        // Strip Ethernet CRC

// Buffer Sizes
#define RCTL_BSIZE_256   (UINT32_C(3) << 16)
#define RCTL_BSIZE_512   (UINT32_C(2) << 16)
#define RCTL_BSIZE_1024  (UINT32_C(1) << 16)
#define RCTL_BSIZE_2048  (UINT32_C(0) << 16)
#define RCTL_BSIZE_4096  ((UINT32_C(3) << 16) | (UINT32_C(1) << 25))
#define RCTL_BSIZE_8192  ((UINT32_C(2) << 16) | (UINT32_C(1) << 25))
#define RCTL_BSIZE_16384 ((UINT32_C(1) << 16) | (UINT32_C(1) << 25))

// Transmit Command
#define TXD_CMD_EOP  (UINT8_C(1) << 0) // End of Packet
#define TXD_CMD_IFCS (UINT8_C(1) << 1) // Insert FCS
#define TXD_CMD_IC   (UINT8_C(1) << 2) // Insert Checksum
#define TXD_CMD_RS   (UINT8_C(1) << 3) // Report Status
#define TXD_CMD_RPS  (UINT8_C(1) << 4) // Report Packet Sent
#define TXD_CMD_VLE  (UINT8_C(1) << 6) // VLAN Packet Enable
#define TXD_CMD_IDE  (UINT8_C(1) << 7) // Interrupt Delay Enable

// TCTL Register
#define TCTL_EN         (UINT32_C(1) << 1)
#define TCTL_PSP        (UINT32_C(1) << 3)
#define TCTL_CT_SHIFT   4
#define TCTL_COLD_SHIFT 12
#define TCTL_SWXOFF     (UINT32_C(1) << 22)
#define TCTL_RTLC       (UINT32_C(1) << 24)

#define TXD_STATUS_DD (UINT8_C(1) << 0) // Descriptor Done
#define TXD_STATUS_EC (UINT8_C(1) << 1) // Excess Collisions
#define TXD_STATUS_LC (UINT8_C(1) << 2) // Late Collision
#define TXD_STATUS_TU (UINT8_C(1) << 3) // Transmit Underrun

#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8

#define E1000_READ(nic, reg)                                                   \
  (*((volatile u32 *)((nic)->mmio_base + (reg))))
#define E1000_WRITE(nic, reg, val)                                             \
  (*((volatile u32 *)((nic)->mmio_base + (reg))) = (val))

#include <stdint.h>

struct e1000_dev {
  u16 bus;
  u16 device;
  u16 function;

  volatile u8 *mmio_base;

  struct e1000_rx_desc *rx_ring_virt;
  uintptr_t rx_ring_phys;             // Programmed into REG_RXDESCLO.
  uintptr_t rx_buffer_phys[E1000_NUM_RX_DESC];
  void *rx_buffers[E1000_NUM_RX_DESC];
  u32 rx_current;

  struct e1000_tx_desc *tx_ring_virt;
  uintptr_t tx_ring_phys;
  uintptr_t tx_buffer_phys[E1000_NUM_TX_DESC];
  void *tx_buffers[E1000_NUM_TX_DESC];
  u32 tx_current;

  u8 mac_addr[6];
};

struct e1000_rx_desc {
  volatile u64 addr;
  volatile u16 len;
  volatile u16 checksum;
  volatile u8 status;
  volatile u8 errors;
  volatile u16 special;
} PACKED;

struct e1000_tx_desc {
  volatile u64 addr;      // 0: Buffer Address
  volatile u16 len;       // 8: Data Length
  volatile u8  cso;       // 10: Checksum Offset (usually 0)
  volatile u8  cmd;       // 11: Command (EOP, IFCS, RS go here)
  volatile u8  status;    // 12: Status (DD bit goes here)
  volatile u8  css;       // 13: Checksum Start (usually 0)
  volatile u16 special;   // 14: Special / VLAN tag (usually 0)
} PACKED;

void e1000_driver_init(void);

#endif // E1000_H
