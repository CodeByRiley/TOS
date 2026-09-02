#ifndef AHCI_H
#define AHCI_H

#include <utilities/types.h>
#include <sync/spinlock.h>
#ifndef QEMU

#else
#define ACHI_
#endif

/* --- AHCI Port Registers --- */
#define AHCI_PORT_PxCLB 0x00  // Command List Base Address
#define AHCI_PORT_PxCLBU 0x04 // Command List Base Address Upper 32-bit
#define AHCI_PORT_PxFB 0x08   // FIS Base Address
#define AHCI_PORT_PxFBU 0x0C  // FIS Base Address Upper 32-bit
#define AHCI_PORT_PxIS 0x10   // Interrupt Status
#define AHCI_PORT_PxIE 0x14   // Interrupt Enable
#define AHCI_PORT_PxCMD 0x18  // Command and Status
#define AHCI_PORT_PxTFD 0x20  // Task File Data
#define AHCI_PORT_PxSIG 0x24  // Signature
#define AHCI_PORT_PxSSTS 0x28 // Serial ATA Status
#define AHCI_PORT_PxSCTL 0x2C // Serial ATA Control
#define AHCI_PORT_PxSERR 0x30 // Serial ATA Error
#define AHCI_PORT_PxSACT 0x34 // Serial ATA Active
#define AHCI_PORT_PxCI 0x38   // Command Issue

/* PxCMD (Command and Status) Register Bits */
#define AHCI_PxCMD_ST (1u << 0)  // Start
#define AHCI_PxCMD_FRE (1u << 4) // FIS Receive Enable
#define AHCI_PxCMD_FR (1u << 14) // FIS Receive Running (Read-only)
#define AHCI_PxCMD_CR (1u << 15) // Command List Running (Read-only)

/* PxSSTS (SATA Status) Register Bits */
#define AHCI_PxSSTS_DET_MASK 0x0F
#define AHCI_PxSSTS_DET_PRESENT 0x03 // Device detected and interface active

/* PxTFD (Task File Data) , low byte is the ATA status register */
#define AHCI_PxTFD_STS_ERR (1u << 0)
#define AHCI_PxTFD_STS_DRQ (1u << 3)
#define AHCI_PxTFD_STS_BSY (1u << 7)

/* PxIS (Interrupt Status) */
#define AHCI_PxIS_TFES (1u << 30) // Task File Error Status

/* Drive signatures reported in PxSIG */
#define AHCI_SIG_SATA 0x00000101u
#define AHCI_SIG_ATAPI 0xEB140101u

enum FIS_TYPE {
  FIS_TYPE_REG_H2D = 0x27,   // Register FIS - host to device
  FIS_TYPE_REG_D2H = 0x34,   // Register FIS - device to host
  FIS_TYPE_DMA_ACT = 0x39,   // DMA activate FIS - device to host
  FIS_TYPE_DMA_SETUP = 0x41, // DMA setup FIS - bidirectional
  FIS_TYPE_DATA = 0x46,      // Data FIS - bidirectional
  FIS_TYPE_BIST = 0x58,      // BIST activate FIS - bidirectional
  FIS_TYPE_PIO_SETUP = 0x5F, // PIO setup FIS - device to host
  FIS_TYPE_DEV_BITS = 0xA1,  // Set device bits FIS - device to host
};

struct FIS_REG_H2D {
  u8 fis_type;   // FIS_TYPE_REG_H2D
  u8 pmport : 4; // Port multiplier
  u8 rsv0 : 3;   // Reserved
  u8 c : 1;      // 1: Command, 0: Control
  u8 command;    // Command register
  u8 featurel;   // Feature register, 7:0
  u8 lba0;       // LBA low register, 7:0
  u8 lba1;       // LBA mid register, 15:8
  u8 lba2;       // LBA high register, 23:16
  u8 device;     // Device register
  u8 lba3;       // LBA register, 31:24
  u8 lba4;       // LBA register, 39:32
  u8 lba5;       // LBA register, 47:40
  u8 featureh;   // Feature register, 15:8
  u8 countl;     // Count register, 7:0
  u8 counth;     // Count register, 15:8
  u8 icc;        // Isochronous command completion
  u8 control;    // Control register
  u8 rsv1[4];    // Reserved
};

struct FIS_REG_D2H {
  u8 fis_type;   // FIS_TYPE_REG_D2H
  u8 pmport : 4; // Port multiplier
  u8 rsv0 : 2;   // Reserved
  u8 i : 1;      // Interrupt bit
  u8 rsv1 : 1;   // Reserved
  u8 status;     // Status register
  u8 error;      // Error register
  u8 lba0;       // LBA low register, 7:0
  u8 lba1;       // LBA mid register, 15:8
  u8 lba2;       // LBA high register, 23:16
  u8 device;     // Device register
  u8 lba3;       // LBA register, 31:24
  u8 lba4;       // LBA register, 39:32
  u8 lba5;       // LBA register, 47:40
  u8 rsv2;       // Reserved
  u8 countl;     // Count register, 7:0
  u8 counth;     // Count register, 15:8
  u8 rsv3[2];    // Reserved
  u8 rsv4[4];    // Reserved
};

struct FIS_DATA {
  u8 fis_type;   // FIS_TYPE_DATA
  u8 pmport : 4; // Port multiplier
  u8 rsv0 : 4;   // Reserved
  u8 rsv1[2];    // Reserved
  u32 data[1];   // Payload
};

struct FIS_PIO_SETUP {
  u8 fis_type;   // FIS_TYPE_PIO_SETUP
  u8 pmport : 4; // Port multiplier
  u8 rsv0 : 1;   // Reserved
  u8 d : 1;      // Data transfer direction, 1 - device to host
  u8 i : 1;      // Interrupt bit
  u8 rsv1 : 1;
  u8 status;   // Status register
  u8 error;    // Error register
  u8 lba0;     // LBA low register, 7:0
  u8 lba1;     // LBA mid register, 15:8
  u8 lba2;     // LBA high register, 23:16
  u8 device;   // Device register
  u8 lba3;     // LBA register, 31:24
  u8 lba4;     // LBA register, 39:32
  u8 lba5;     // LBA register, 47:40
  u8 rsv2;     // Reserved
  u8 countl;   // Count register, 7:0
  u8 counth;   // Count register, 15:8
  u8 rsv3;     // Reserved
  u8 e_status; // New value of status register
  u16 tc;      // Transfer count
  u8 rsv4[2];  // Reserved
};

struct FIS_DMA_SETUP {
  u8 fis_type;       // FIS_TYPE_DMA_SETUP
  u8 pmport : 4;     // Port multiplier
  u8 rsv0 : 1;       // Reserved
  u8 d : 1;          // Data transfer direction, 1 - device to host
  u8 i : 1;          // Interrupt bit
  u8 a : 1;          // Auto-activate. Specifies if DMA Activate FIS is needed
  u8 rsved[2];       // Reserved
  u64 DMAbufferID;   // DMA Buffer Identifier. Used to Identify DMA buffer in
                     // host memory. SATA Spec says host specific and not in
                     // Spec. Trying AHCI spec might work.
  u32 rsvd;          // More reserved
  u32 DMAbufOffset;  // Byte offset into buffer. First 2 bits must be 0
  u32 TransferCount; // Number of bytes to transfer. Bit 0 must be 0
  u32 resvd;         // Reserved
};

/* --- Command Structures --- */
/* Physical Region Descriptor Table (Scatter/Gather for DMA) */
struct AHCI_PHYS_DESC {
  u32 data_base; // Physical address of data buffer
  u32 data_base_upper;
  u32 reserved;
  u32 size; // Bit 31 = Interrupt on Completion. Bits 30:0 = size in bytes - 1
};

/* Command Header (Points to the Command Table).
 *
 * The HBA indexes the command list with a fixed 32-byte stride and reads
 * PRDTL out of the upper half of DW0, so this layout has to match the AHCI
 * spec bit-for-bit. Bitfields are allocated LSB-first on x86, giving:
 *   DW0 bits 4:0 CFL, 5 A, 6 W, 7 P, 8 R, 9 B, 10 C, 11 rsv, 15:12 PMP,
 *       31:16 PRDTL
 *   DW1 PRDBC, DW2/DW3 CTBA/CTBAU, DW4-DW7 reserved. */
struct AHCI_CMD_HEADER {
  /* DW0 */
  u8 cfl : 5;    // Command FIS Length (in DWORDs)
  u8 atapi : 1;
  u8 write : 1;  // 1 = Write to device, 0 = Read from device
  u8 prefetch : 1;

  u8 reset : 1;
  u8 bist : 1;
  u8 c : 1;      // Clear busy upon R_OK
  u8 rsv0 : 1;
  u8 pmp : 4;    // Port multiplier

  u16 prdt_length;    // Number of PRDT entries
  /* DW1 */
  volatile u32 prdbc; // Bytes transferred; written by the HBA
  /* DW2-DW3 */
  u32 command_table_base;
  u32 command_table_base_upper;
  /* DW4-DW7 */
  u32 rsv1[4];
} PACKED;

_Static_assert(sizeof(struct AHCI_CMD_HEADER) == 32,
               "AHCI command header must be exactly 32 bytes");

#define AHCI_PRDT_ENTRIES 8

/* Command Table (Holds the FIS and the PRDT array).
 * Spec offsets: CFIS at 0x00 (64 bytes), ACMD at 0x40, PRDT at 0x80.
 * Eight PRDT entries round the whole thing to 256 bytes, so a 4 KiB frame
 * holds 16 of them and every one lands on the required 128-byte alignment. */
struct AHCI_CMD_TABLE {
  struct FIS_REG_H2D command_fis; // 0x00, 20 bytes
  u8 rsv0[44];                    // Pad the CFIS area out to 64 bytes
  u8 acmd[16];                    // 0x40, ATAPI command
  u8 rsv1[48];                    // 0x50..0x7F reserved
  struct AHCI_PHYS_DESC prdt[AHCI_PRDT_ENTRIES]; // 0x80
} PACKED;

_Static_assert(sizeof(struct AHCI_CMD_TABLE) == 256,
               "AHCI command table must be exactly 256 bytes");

/* Command tables per port, bounded by what fits in one 4 KiB frame. */
#define AHCI_CMD_TABLE_STRIDE 256u
#define AHCI_SLOTS_PER_PORT (4096u / AHCI_CMD_TABLE_STRIDE)

#define AHCI_MAX_PORTS 32

/* State and memory allocations for a single SATA port */
struct AHCI_PORT {
    struct spinlock io_lock; // Serialize commands, including cache barriers.
    int is_active;          // 1 if a drive is present and initialized
    u32 signature;     // 0x00000101 for SATA, 0xEB140101 for ATAPI

    void *cmd_list_virt;    // Virtual address of the 1KB Command List
    u64 cmd_list_phys; // Physical address of the Command List

    void *fis_virt;         // Virtual address of the 256B Received FIS
    u64 fis_phys;      // Physical address of the Received FIS

    void *cmd_table_virt;   // Virtual base of the AHCI_SLOTS_PER_PORT tables
    u64 cmd_table_phys;// Physical base of the Command Table array

    u32 slots;              // Usable command slots (min of HBA NCS and array)
};

/* Main device structure holding HBA state and all ports */
struct AHCI_DEVICE_DATA {
    uptr abar_phys;                 // Physical address of AHCI Base Address (BAR5)
    void *abar_virtual;             // Virtual memory mapping of ABAR
    u32 ports_implemented;     // Bitmap of ports implemented by HBA
    int irq;                        // IRQ number (if using legacy interrupts)

    struct AHCI_PORT ports[AHCI_MAX_PORTS]; // Array of port states
};

void ahci_init(void);

/* Read `count` 512-byte sectors starting at `lba` into the physical buffer
 * `buf_phys`. Returns 0 on success, -1 on error or timeout. */
int ahci_read_sector(struct AHCI_DEVICE_DATA *dev, int port, u64 lba,
                     u32 count, void *buf_phys);

/* Write `count` 512-byte sectors starting at `lba` from the physical buffer
 * `buf_phys`. Returns 0 on success, -1 on error or timeout. */
int ahci_write_sector(struct AHCI_DEVICE_DATA *dev, int port, u64 lba,
                      u32 count, void *buf_phys);

int ahci_identify(struct AHCI_DEVICE_DATA *dev, int port, void *buf_phys);
int ahci_flush_cache(struct AHCI_DEVICE_DATA *dev, int port);

#endif
