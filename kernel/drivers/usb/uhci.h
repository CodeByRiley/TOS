#ifndef UHCI_H
#define UHCI_H

#include <utilities/types.h>

/* UHCI I/O Registers */
// Offset | Name 			 | Description 					   | Length
// 00			| USBCMD		 | USB Command	 					 | 2 bytes
// 02			| USBSTS		 | USB Status	 					   | 2 bytes
// 04			| USBINTR		 | USB Interrupt Enable	   | 2 bytes
// 06			| FRNUM		   | Frame Number	           | 2 bytes
// 08			| FRBASEADD	 | Frame List Base Address | 4 bytes
// 0C			| SOFMOD		 | Start Of Frame Modify	 | 1 byte
// 10			| PORTSC1		 | Port 1 Status/Control	 | 2 bytes
// 12			| PORTSC2		 | Port 2 Status/Control	 | 2 bytes
#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FRBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

/* UHCI USB Command Register (USBCMD) */
// Bits | Name 												    											 | Description
// 15-8 | Reserved                                                |
// 7    | Max Packet Size (MAXP)                                  | 0 = Max Size is 32 bytes, 1 = Max Size is 64 bytes
// 6    | Configure Flag (CF)                                     | Host sets after it configures the host controller (no hardware effect)
// 5    | Software Debug (SWDBG)                                  | 0 = Normal mode, 1 = Debug mode (controller clears Run after each transaction)
// 4    | Force Global Resume (FGR)                               | 1 = Controller sends Global Resume signal on the USB
// 3    | Enter Global Suspend Mode (EGSM)                        | 1 = Controller enters Global Suspend mode (suspends USB transactions)
// 2    | Global Reset (GRESET)                                   | 1 = Controller sends Global Reset signal on the USB and resets itself
// 1    | Host Controller Reset (HCRESET)                         | 1 = Controller resets its internal state to initial values (bit clears after reset is done)
// 0    | Run/Stop (RS)                                           | 1 = Controller executes Frame List Entries, 0 = Controller halts
#define UHCI_USBCMD_RS        (1u << 0)   // Run/Stop
#define UHCI_USBCMD_HCRESET   (1u << 1)   // Host Controller Reset (self-clears)
#define UHCI_USBCMD_GRESET    (1u << 2)   // Global Reset (USB reset)
#define UHCI_USBCMD_EGSM      (1u << 3)   // Enter Global Suspend Mode
#define UHCI_USBCMD_FGR       (1u << 4)   // Force Global Resume
#define UHCI_USBCMD_SWDBG     (1u << 5)   // Software Debug
#define UHCI_USBCMD_CF        (1u << 6)   // Configure Flag
#define UHCI_USBCMD_MAXP      (1u << 7)   // Max Packet Size: 0=32, 1=64

/* UHCI USB Status Register (USBSTS) */
//
// Bits | Name 												    											| Description
// 15-6 | Reserved                                              |
// 5    | HCHalted                                              | Set to 1 by Host Controller after it stops executing the Frame List
// 4    | Host Controller Process Error                         | Set to 1 by Host Controller when it encounters a fatal error while processing a TD
// 3    | Host System Error                                     | Set to 1 by Host Controller when a host system access error occurs (e.g. PCI parity error)
// 2    | Resume Detected                                       | Set to 1 by Host Controller when it receives a RESUME signal from a USB device
// 1    | USB Error Interrupt                                   | Set to 1 by Host Controller when a USB transaction results in an error
// 0    | USB Interrupt                                         | Set to 1 by Host Controller after it completes a USB transaction whose TD has its IOC bit set
#define UHCI_USBSTS_INTERRUPT        (1u << 0)   // USB Interrupt
#define UHCI_USBSTS_USBERR           (1u << 1)   // USB Error Interrupt
#define UHCI_USBSTS_RESUME          (1u << 2)   // Resume Detected
#define UHCI_USBSTS_HOST_SYS_ERR    (1u << 3)   // Host System Error
#define UHCI_USBSTS_PROCESS_ERR     (1u << 4)   // Host Controller Process Error
#define UHCI_USBSTS_HCHALTED         (1u << 5)   // HCHalted

/* UHCI USB Interrupt Enable Register (USBINTR) */
//
// Bits | Name 												     											| Description
// 15-4 | Reserved                                              |
// 3    | Short Packet Interrupt Enable                         | Enable interrupt when a TD completes with short packet condition
// 2    | Interrupt on Complete Enable                          | Enable interrupts on TD completion when the TD has its IOC bit set
// 1    | Resume Interrupt Enable                               | Enable interrupt when resume is detected
// 0    | Timeout/CRC Interrupt Enable                          | Enable interrupt on timeout/CRC related TD completion conditions
#define UHCI_USBINTR_TIMEOUT_CRC_EN (1u << 0)
#define UHCI_USBINTR_RESUME_EN       (1u << 1)
#define UHCI_USBINTR_IOC_EN          (1u << 2)   // Interrupt on Complete (IOC)
#define UHCI_USBINTR_SHORT_EN       (1u << 3)

/* UHCI Port Status/Control (PORTSC1, PORTSC2 , identical layout) */
//
// Bits | Name                              | Description
// 15-13| Reserved                          |
// 12   | Suspend (R/W)                     | 1 = Port suspended, no traffic forwarded
// 11-10| Reserved                          |
// 9    | Port Reset (R/W)                  | 1 = Drive USB reset on the port; software clears it
// 8    | Low Speed Device Attached (RO)    | 1 = Attached device is low speed
// 7    | Reserved                          | Always reads 1, so an idle empty port reads 0x0080
// 6    | Resume Detect (R/W)               | 1 = Resume signalling detected/driven
// 5    | Line Status D- (RO)               | Raw D- line state, valid regardless of enable
// 4    | Line Status D+ (RO)               | Raw D+ line state, valid regardless of enable
// 3    | Port Enable Change (R/WC)         | 1 = Port Enable changed; write 1 to acknowledge
// 2    | Port Enable (R/W)                 | 1 = Port passes traffic; cleared by reset
// 1    | Connect Status Change (R/WC)      | 1 = Connect Status changed; write 1 to acknowledge
// 0    | Current Connect Status (RO)       | 1 = A device is attached
//
// PE and PEC sit below the line-status pair, not above it. Defining PE as
// bit 4 aims the enable write at a read-only line-status bit: the port never
// enables and the controller reports no error, it just ignores the write.
#define UHCI_PORTSC_CCS        (1u << 0)  // Current Connect Status (RO)
#define UHCI_PORTSC_CSC        (1u << 1)  // Connect Status Change (R/WC)
#define UHCI_PORTSC_PE         (1u << 2)  // Port Enable (R/W)
#define UHCI_PORTSC_PEC        (1u << 3)  // Port Enable Change (R/WC)
#define UHCI_PORTSC_LS_SHIFT   4          // Line Status: D+ (4), D- (5) (RO)
#define UHCI_PORTSC_LS_MASK    (3u << UHCI_PORTSC_LS_SHIFT)
#define UHCI_PORTSC_RD         (1u << 6)  // Resume Detect (R/W)
#define UHCI_PORTSC_RES1       (1u << 7)  // Reserved, always reads 1
#define UHCI_PORTSC_LSDA       (1u << 8)  // Low-Speed Device Attached (RO)
#define UHCI_PORTSC_PR         (1u << 9)  // Port Reset (R/W)
#define UHCI_PORTSC_SUSP       (1u << 12) // Suspend (R/W)

/* Write-1-to-clear. A read-modify-write of PORTSC must mask these out of the
 * value written, or it acknowledges a change it never looked at. */
#define UHCI_PORTSC_WC_MASK    (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)

/* Link Pointer (uhci_frame.link_ptr, uhci_qh.*_ptr, uhci_tdn.link_ptr) */
//
// Bits | Name                | Description
// 31-4 | Address             | Physical address of the next QH/TD
// 3    | Reserved            |
// 2    | Depth First (Vf)    | 1 = Follow this chain now, 0 = one TD per frame
// 1    | QH/TD Select (Q)    | 1 = Address points to a QH, 0 = to a TD
// 0    | Terminate (T)       | 1 = End of list, address ignored
//
// Bits 3-0 are stolen from the address, so every QH and TD must be 16-byte
// aligned or its address collides with the control bits.
#define UHCI_PTR_TERM     (1u << 0)
#define UHCI_PTR_QH       (1u << 1)
#define UHCI_PTR_DEPTH    (1u << 2)

/* TD Token (uhci_td.token) , DWORD 2 */
//
// Bits  | Name              | Description
// 31-21 | Maximum Length    | Bytes to transfer, encoded as (len - 1); 0x7FF = 0 bytes
// 20    | Reserved          |
// 19    | Data Toggle       | DATA0/DATA1 sequence bit for this packet
// 18-15 | Endpoint          | Target endpoint number
// 14-8  | Device Address    | Target device address (0 until SET_ADDRESS lands)
// 7-0   | Packet ID (PID)   | SETUP / IN / OUT
#define UHCI_TD_PID(pid)        ((u32)((pid) & 0xFF))
#define UHCI_TD_DEVADDR(addr)   ((u32)((addr) & 0x7F) << 8)
#define UHCI_TD_ENDPOINT(ep)    ((u32)((ep) & 0xF) << 15)
#define UHCI_TD_TOGGLE(t)       ((u32)((t) & 0x1) << 19)
#define UHCI_TD_MAXLEN(len)     ((u32)(((len) - 1) & 0x7FF) << 21)

/* Common USB PIDs */
#define USB_PID_SETUP   0x2D
#define USB_PID_IN      0x69
#define USB_PID_OUT     0xE1

/* TD Control and Status (uhci_td.status) , DWORD 1 */
//
// Bits  | Name                    | Description
// 31-30 | Reserved                |
// 29    | Short Packet Detect     | 1 = A short IN packet retires the queue instead of continuing
// 28-27 | Error Counter (C_ERR)   | Retries left; 0 = no limit. Decremented on error, not on NAK
// 26    | Low Speed Device        | 1 = Target is a low-speed device
// 25    | Isochronous Select      | 1 = Isochronous TD
// 24    | Interrupt on Complete   | 1 = Raise USBSTS.INTERRUPT when this TD retires
// 23    | Active                  | 1 = Controller owns this TD; it clears the bit when done
// 22    | Stalled                 | 1 = Endpoint stalled, or retries exhausted
// 21    | Data Buffer Error       | 1 = Host could not keep up with the buffer
// 20    | Babble Detected         | 1 = Device sent more than it was allowed
// 19    | NAK Received            | 1 = Device not ready; TD stays Active and is retried
// 18    | CRC/Time Out Error      | 1 = No response, or the response was corrupt
// 17    | Bitstuff Error          | 1 = Bitstuffing violation on the wire
// 16-11 | Reserved                |
// 10-0  | Actual Length           | Bytes actually moved, encoded as (len - 1)
//
// Bits 16-11 are the trap here. Omitting them shifts every flag down by five,
// landing Active on CRC/Timeout and C_ERR on Stalled|Active , the controller
// then gets a TD claiming to be simultaneously active and stalled, and the
// driver reads a freshly-set error flag as "still running".
//
// The controller DMA-writes this word, so read it with one volatile u32 load
// rather than field by field; otherwise the compiler may reload mid-decode
// and mix status from two different instants.
#define UHCI_TD_STS_ACTLEN_MASK 0x7FF
#define UHCI_TD_STS_BITSTUFF    (1 << 17)
#define UHCI_TD_STS_CRCTIMEOUT  (1 << 18)
#define UHCI_TD_STS_NAK         (1 << 19)
#define UHCI_TD_STS_BABBLE      (1 << 20)
#define UHCI_TD_STS_BUFERR      (1 << 21)
#define UHCI_TD_STS_STALLED     (1 << 22)
#define UHCI_TD_STS_ACTIVE      (1 << 23)
#define UHCI_TD_STS_IOC         (1 << 24)
#define UHCI_TD_STS_ISO         (1 << 25)
#define UHCI_TD_STS_LS          (1 << 26)
#define UHCI_TD_STS_CERR_SHIFT  27
#define UHCI_TD_STS_SPD         (1 << 29)

/* Fatal conditions. NAK is excluded: the controller retries a NAKed TD on its
 * own while C_ERR lasts, so mid-poll it means "not ready yet", not "failed". */
#define UHCI_TD_STS_ERRMASK                                                    \
  (UHCI_TD_STS_BITSTUFF | UHCI_TD_STS_CRCTIMEOUT | UHCI_TD_STS_BABBLE |        \
   UHCI_TD_STS_BUFERR | UHCI_TD_STS_STALLED)

/* Decode Actual Length. Stored as (bytes - 1), so 0x7FF means zero moved. */
#define UHCI_TD_ACTLEN(sts)                                                    \
  ((((sts) & UHCI_TD_STS_ACTLEN_MASK) + 1) & UHCI_TD_STS_ACTLEN_MASK)

struct pci_device;

/* Frame list entry. The list is 1024 of these and must be 4KB aligned. */
struct uhci_frame {
	u32 link_ptr;
} PACKED;

/* Queue Head (8 bytes, 16-byte aligned).
 * link_ptr chains to the next QH; element_ptr is the work queue, which the
 * controller advances as TDs retire and leaves at TERM when drained. */
struct uhci_qh {
	u32 link_ptr;
	u32 element_ptr;
} PACKED;

/* TD link word (DWORD 0). */
struct uhci_tdn {
	u32 link_ptr;
} PACKED;

/* TD status word (DWORD 1), as bitfields. Layout and the reasoning behind it
 * are documented at UHCI_TD_STS_* above; access it through those masks, not
 * these fields, since the controller writes it by DMA. */
struct uhci_tds {
	u32 actual_length : 11;
  u32 reserved1     : 6;
  u32 bitstuff_err  : 1;
  u32 crc_timeout   : 1;
  u32 nak_received  : 1;
  u32 babble        : 1;
  u32 buffer_err    : 1;
  u32 stalled       : 1;
  u32 active        : 1;
  u32 ioc           : 1;
  u32 isochronous   : 1;
  u32 low_speed     : 1;
  u32 error_counter : 2;
  u32 short_packet  : 1;
  u32 reserved2     : 2;
} PACKED;

/* Transfer Descriptor (16 bytes, 16-byte aligned). buffer_ptr is a physical
 * address the controller DMAs to or from, so it must be under 4 GiB. */
struct uhci_td {
	struct uhci_tdn link;
  struct uhci_tds status;
  u32 token;
  u32 buffer_ptr;
} PACKED;

int uhci_init(struct pci_device *dev);

#endif // UHCI_H
