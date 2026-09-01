#ifndef EHCI_H
#define EHCI_H

#include <drivers/base/helpers.h>
#include <drivers/base/macros.h>
#include <stddef.h>
#include <stdint.h>
#include <utilities/types.h>

/* EHCI Capability Register offsets */
#define EHCI_CAPLENGTH UINT32_C(0x00)
#define EHCI_HCIVERSION UINT32_C(0x02)
#define EHCI_HCSPARAMS UINT32_C(0x04)
#define EHCI_HCCPARAMS UINT32_C(0x08)
#define EHCI_HCSP_PORTROUTE UINT32_C(0x0C)

/*
 * EHCI Operational Register offsets.
 *
 * These are relative to the operational register base:
 *
 *     op_base = ehci_base + CAPLENGTH
 *
 * Do not assume op_base is always ehci_base + 0x10.
 */
#define EHCI_USBCMD UINT32_C(0x00)
#define EHCI_USBSTS UINT32_C(0x04)
#define EHCI_USBINTR UINT32_C(0x08)
#define EHCI_FRINDEX UINT32_C(0x0C)
#define EHCI_CTRLDSSEGMENT UINT32_C(0x10)
#define EHCI_PERIODICLISTBASE UINT32_C(0x14)
#define EHCI_ASYNCLISTADDR UINT32_C(0x18)
#define EHCI_CONFIGFLAG UINT32_C(0x40)

#define EHCI_PORTSC_BASE UINT32_C(0x44)
/* Port n's PORTSC register, where n is zero-based. */
#define EHCI_PORTSC(n) (EHCI_PORTSC_BASE + UINT32_C(4) * (n))

/* USBCMD single-bit fields */
#define CMD_REG_RUN_STOP (UINT32_C(1) << 0)
#define CMD_REG_HOST_CONTROLLER_RESET (UINT32_C(1) << 1)
#define CMD_REG_PERIODIC_SCHEDULE_ENABLE (UINT32_C(1) << 4)
#define CMD_REG_ASYNC_SCHEDULE_ENABLE (UINT32_C(1) << 5)
#define CMD_REG_INTERRUPT_ON_ASYNC_ADVANCE_DOORBELL (UINT32_C(1) << 6)
#define CMD_REG_LIGHT_HOST_CONTROLLER_RESET (UINT32_C(1) << 7)
#define CMD_REG_ASYNC_SCHEDULE_PARK_MODE_ENABLE (UINT32_C(1) << 11)
/* USBCMD multi-bit fields */
#define CMD_REG_FRAME_LIST_SIZE FIELD_MASK(2, 2)                /* bits 3–2 */
#define CMD_REG_ASYNC_SCHEDULE_PARK_MODE_COUNT FIELD_MASK(2, 8) /* bits 9–8 */
#define CMD_REG_INTERRUPT_THRESHOLD FIELD_MASK(8, 16)           /* bits 23–16 */
/* USBCMD reserved fields */
#define CMD_REG_RESERVED_10 (UINT32_C(1) << 10)
#define CMD_REG_RESERVED_15_12 FIELD_MASK(4, 12)
#define CMD_REG_RESERVED_31_24 FIELD_MASK(8, 24)

/* USBSTS register */
/* Status bits */
#define STS_REG_INTERRUPT (UINT32_C(1) << 0)
#define STS_REG_ERROR_INTERRUPT (UINT32_C(1) << 1)
#define STS_REG_PORT_CHANGE_DETECT (UINT32_C(1) << 2)
#define STS_REG_FRAME_LIST_ROLLOVER (UINT32_C(1) << 3)
#define STS_REG_HOST_SYSTEM_ERROR (UINT32_C(1) << 4)
#define STS_REG_INTERRUPT_ON_ASYNC_ADVANCE (UINT32_C(1) << 5)
#define STS_REG_HOST_CONTROLLER_HALTED (UINT32_C(1) << 12)
#define STS_REG_RECLAMATION (UINT32_C(1) << 13)
#define STS_REG_PERIODIC_SCHEDULE_STATUS (UINT32_C(1) << 14)
#define STS_REG_ASYNC_SCHEDULE_STATUS (UINT32_C(1) << 15)
/* USBSTS reserved fields */
#define STS_REG_RESERVED_11_6 FIELD_MASK(6, 6)
#define STS_REG_RESERVED_31_16 FIELD_MASK(16, 16)

/* USBINTR register */
#define INTR_REG_INTERRUPT_ENABLE (UINT32_C(1) << 0)
#define INTR_REG_ERROR_INTERRUPT_ENABLE (UINT32_C(1) << 1)
#define INTR_REG_PORT_CHANGE_INTERRUPT_ENABLE (UINT32_C(1) << 2)
#define INTR_REG_FRAME_LIST_ROLLOVER_ENABLE (UINT32_C(1) << 3)
#define INTR_REG_HOST_SYSTEM_ERROR_ENABLE (UINT32_C(1) << 4)
#define INTR_REG_INTERRUPT_ON_ASYNC_ADVANCE_ENABLE (UINT32_C(1) << 5)
/* USBINTR reserved fields */
#define INTR_REG_RESERVED_31_6 FIELD_MASK(26, 6)

/* PORTSC register */
#define PORTSC_REG_CONNECT_STATUS (UINT32_C(1) << 0)
#define PORTSC_REG_CONNECT_STATUS_CHANGE                                       \
  (UINT32_C(1) << 1) /* Write 1 to clear */
#define PORTSC_REG_ENABLE (UINT32_C(1) << 2)
#define PORTSC_REG_ENABLE_CHANGE (UINT32_C(1) << 3) /* Write 1 to clear */
#define PORTSC_REG_OVERCURRENT_ACTIVE (UINT32_C(1) << 4)
#define PORTSC_REG_OVERCURRENT_CHANGE (UINT32_C(1) << 5) /* Write 1 to clear   \
                                                          */
#define PORTSC_REG_FORCE_PORT_RESUME (UINT32_C(1) << 6)
#define PORTSC_REG_SUSPEND (UINT32_C(1) << 7)
#define PORTSC_REG_RESET (UINT32_C(1) << 8)
#define PORTSC_REG_LINE_STATUS_MASK FIELD_MASK(2, 10) /* bits 11–10 */
#define PORTSC_REG_PORT_POWER (UINT32_C(1) << 12)
#define PORTSC_REG_OWNER (UINT32_C(1) << 13)
#define PORTSC_REG_INDICATOR_CONTROL FIELD_MASK(2, 14)      /* bits 15–14 */
#define PORTSC_REG_PORT_TEST_CONTROL_MASK FIELD_MASK(4, 16) /* bits 19–16 */
#define PORTSC_REG_WAKE_ON_CONNECT_ENABLE (UINT32_C(1) << 20)
#define PORTSC_REG_WAKE_ON_DISCONNECT_ENABLE (UINT32_C(1) << 21)
#define PORTSC_REG_WAKE_ON_OVERCURRENT_ENABLE (UINT32_C(1) << 22)
#define PORTSC_REG_RESERVED_31_23 FIELD_MASK(9, 23)
#define PORTSC_REG_WC_MASK                                                   \
  (PORTSC_REG_CONNECT_STATUS_CHANGE | PORTSC_REG_ENABLE_CHANGE |             \
   PORTSC_REG_OVERCURRENT_CHANGE)

/* HCSPARAMS capability fields */
#define HCSPARAMS_REG_N_PORTS FIELD_MASK(4, 0) /* bits 3–0 */
#define HCSPARAMS_REG_PPC (UINT32_C(1) << 4)   /* Port Power Control */
#define HCSPARAMS_REG_PORT_ROUTING_RULES (UINT32_C(1) << 7)
#define HCSPARAMS_REG_N_PCC FIELD_MASK(4, 8)      /* bits 11–8 */
#define HCSPARAMS_REG_N_CC FIELD_MASK(4, 12)      /* bits 15–12 */
#define HCSPARAMS_REG_PI (UINT32_C(1) << 16)      /* Port Indicators */
#define HCSPARAMS_REG_PARKING (UINT32_C(1) << 17) /* Light Host Controller */
/* Remaining bits are reserved */
#define HCSPARAMS_REG_RESERVED_31_18 FIELD_MASK(14, 18)

/* HCCPARAMS capability fields */
#define HCCPARAMS_REG_64_BIT_ADDRESSING (UINT32_C(1) << 0)
#define HCCPARAMS_REG_PROG_FRAME_LIST_FLAG (UINT32_C(1) << 1)
#define HCCPARAMS_REG_ASYNC_SCHEDULE_PARK_CAP (UINT32_C(1) << 2)
#define HCCPARAMS_REG_ISOCHRONOUS_SCHED_THRESHOLD                              \
  FIELD_MASK(4, 4)                          /* bits 7–4 */
#define HCCPARAMS_REG_EECP FIELD_MASK(8, 8) /* bits 15–8 */
#define HCCPARAMS_REG_RESERVED_31_16 FIELD_MASK(16, 16)
#define CTRLDSSEGMENT_REG_BASE UINT32_C(0xFFFFFFFF)
#define PERIODICLISTBASE_REG_BASE UINT32_C(0xFFFFF000)
#define ASYNCLISTADDR_REG_ADDRESS UINT32_C(0xFFFFFFE0)

/* CONFIGFLAG register */
#define CONFIGFLAG_REG_CONFIGURED (UINT32_C(1) << 0)
#define CONFIGFLAG_REG_RESERVED_31_1 FIELD_MASK(31, 1)

/* Shared link pointer format (frame list entries, QH/QTD chains) */
#define LINK_TERMINATE UINT32_C(1)
#define LINK_TYPE_QH                                                           \
  (UINT32_C(1) << 1) /* bits 2-1: 00 iTD, 01 QH, 10 siTD, 11 FSTN */
#define LINK_ADDR_MASK UINT32_C(0xFFFFFFE0) /* 32-byte aligned pointers */

/* QH endpoint characteristics (dword 1) */
#define QH_EPCHAR_ADDR_MASK      FIELD_MASK(7, 0)    /* bits 6-0: device address */
#define QH_EPCHAR_I              (UINT32_C(1) << 7)  /* inactive on next transaction */
#define QH_EPCHAR_EP_MASK        FIELD_MASK(4, 8)    /* bits 11-8: endpoint number */
#define QH_EPCHAR_SPEED_MASK     FIELD_MASK(2, 12)   /* bits 13-12 */
#define QH_EPCHAR_SPEED_FULL     (UINT32_C(0) << 12) /* pre-shifted, like the PIDs */
#define QH_EPCHAR_SPEED_LOW      (UINT32_C(1) << 12)
#define QH_EPCHAR_SPEED_HIGH     (UINT32_C(2) << 12)
#define QH_EPCHAR_DTC            (UINT32_C(1) << 14) /* data toggle from qTD */
#define QH_EPCHAR_H              (UINT32_C(1) << 15) /* reclamation-list head */
#define QH_EPCHAR_MAXP_MASK      FIELD_MASK(11, 16)  /* bits 26-16: max packet size */
#define QH_EPCHAR_C              (UINT32_C(1) << 27) /* split control endpoint */
#define QH_EPCHAR_RL_MASK        FIELD_MASK(4, 28)   /* bits 31-28: NAK reload */

/* QH endpoint capabilities (dword 2) */
#define QH_EPCAP_MULT_MASK       FIELD_MASK(2, 30)   /* bits 31-30 */
#define QH_EPCAP_MULT_1          (UINT32_C(1) << 30) /* 1 transaction per microframe */
#define QH_EPCAP_MULT_2          (UINT32_C(2) << 30)
#define QH_EPCAP_MULT_3          (UINT32_C(3) << 30)
#define QH_EPCAP_PORT_MASK       FIELD_MASK(7, 23)   /* bits 29-23: TT port (split) */
#define QH_EPCAP_HUB_MASK        FIELD_MASK(7, 16)   /* bits 22-16: TT hub (split) */
#define QH_EPCAP_SCMASK_MASK     FIELD_MASK(8, 8)    /* bits 15-8: split completion */
#define QH_EPCAP_SMASK_MASK      FIELD_MASK(8, 0)    /* bits 7-0: int schedule mask */

/* qTD token */
#define QTD_PID_OUT (UINT32_C(0) << 8)
#define QTD_PID_IN (UINT32_C(1) << 8)
#define QTD_PID_SETUP (UINT32_C(2) << 8)
#define QTD_PID_MASK FIELD_MASK(2, 8) /* for FIELD_GET when logging tokens */
#define QTD_STATUS_ACTIVE (UINT32_C(1) << 7)
#define QTD_STATUS_HALTED (UINT32_C(1) << 6)
#define QTD_STATUS_BUFFER_ERR (UINT32_C(1) << 5)
#define QTD_STATUS_BABBLE (UINT32_C(1) << 4)
#define QTD_STATUS_XACT_ERR (UINT32_C(1) << 3)
#define QTD_STATUS_MISSED_MFRAME (UINT32_C(1) << 2)
#define QTD_IOC (UINT32_C(1) << 15)
#define QTD_CERR_MASK FIELD_MASK(2, 10)
#define QTD_CURRENT_PAGE_MASK FIELD_MASK(3, 12)
#define QTD_TOTAL_LEN_MASK FIELD_MASK(15, 16)
#define QTD_TOGGLE (UINT32_C(1) << 31)
#define QTD_ERROR_MASK                                                       \
  (QTD_STATUS_HALTED | QTD_STATUS_BUFFER_ERR | QTD_STATUS_BABBLE |           \
   QTD_STATUS_XACT_ERR | QTD_STATUS_MISSED_MFRAME)

/* Legacy support (PCI config space, via EECP) */
#define USBLEGSUP_CAP_ID UINT8_C(0x01)
#define USBLEGSUP_BIOS_OWNED (UINT32_C(1) << 16) /* RO: BIOS owns HC */
#define USBLEGSUP_OS_OWNED (UINT32_C(1) << 24)   /* W1S: OS takes ownership */
#define USBLEGSUP_NEXT_MASK                                                    \
  FIELD_MASK(8, 8)            /* next legacy cap offset, 0 = end */
#define USBLEGCTLSTS_OFFSET 4 /* SMI enable/status, at EECP+4 */

/* The 8 overlay dwords — same shape as a qTD, but WITHOUT the 32-byte
 * alignment attribute, so it can sit at offset 16 inside a QH. */
struct ehci_qh_overlay {
  u32 next;
  u32 alt_next;
  u32 token;
  u32 buf[5];
};

struct ehci_qtd {
  u32 next;
  u32 alt_next;
  u32 token;
  u32 buf[5];
} ALIGNED(32); /* the HC requires each qTD's ADDRESS to be 32-byte aligned */

/* The hardware reads 12 dwords = 48 bytes. sizeof is 64 because C pads
 * the struct to a multiple of its 32-byte alignment; the trailing pad is
 * dead space the controller never touches. */
struct ehci_qh {
  u32 horiz_link;                 /* 0:  next QH | type bits | TERMINATE */
  u32 ep_char;                    /* 4:  addr, ep, speed, MPS, ctrl flag */
  u32 ep_cap;                     /* 8:  mult, hub/port, split masks */
  u32 curr_qtd;                   /* 12: written by the HC */
  struct ehci_qh_overlay overlay; /* 16-47: HC mirror of the active qTD */
} ALIGNED(32);

_Static_assert(sizeof(struct ehci_qtd) == 32, "qTD layout");
_Static_assert(sizeof(struct ehci_qh_overlay) == 32, "overlay layout");
_Static_assert(offsetof(struct ehci_qh, horiz_link) == 0, "qh dword 0");
_Static_assert(offsetof(struct ehci_qh, ep_char) == 4, "qh dword 1");
_Static_assert(offsetof(struct ehci_qh, ep_cap) == 8, "qh dword 2");
_Static_assert(offsetof(struct ehci_qh, curr_qtd) == 12, "qh dword 3");
_Static_assert(offsetof(struct ehci_qh, overlay) == 16, "qh overlay");
_Static_assert(sizeof(struct ehci_qh) == 64, "QH padded layout");

struct pci_device;
int ehci_init(struct pci_device *dev);

#endif /* EHCI_H */
