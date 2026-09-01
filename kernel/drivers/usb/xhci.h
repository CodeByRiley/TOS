
#ifndef XHCI_H
#define XHCI_H

#include <utilities/types.h>
#include <drivers/base/macros.h>

/*
 * xHCI Capability Registers
 *
 * These offsets are relative to the xHCI MMIO base.
 */
#define XHCI_CAPLENGTH UINT32_C(0x00) /* uint8_t */
#define XHCI_HCIVERSION UINT32_C(0x02)
#define XHCI_HCSPARAMS1 UINT32_C(0x04)
#define XHCI_HCSPARAMS2 UINT32_C(0x08)
#define XHCI_HCSPARAMS3 UINT32_C(0x0C)
#define XHCI_HCCPARAMS1 UINT32_C(0x10)
#define XHCI_DBOFF UINT32_C(0x14)
#define XHCI_RTSOFF UINT32_C(0x18)
#define XHCI_HCCPARAMS2 UINT32_C(0x1C)

/*
 * xHCI Operational Registers
 *
 * These are relative to:
 *
 *     op_base = xhci_base + CAPLENGTH
 */
#define XHCI_USBCMD UINT32_C(0x00)
#define XHCI_USBSTS UINT32_C(0x04)
#define XHCI_PAGESIZE UINT32_C(0x08)
#define XHCI_DNCTRL UINT32_C(0x14)
#define XHCI_CRCR UINT32_C(0x18)   /* 64-bit */
#define XHCI_DCBAAP UINT32_C(0x30) /* 64-bit */
#define XHCI_CONFIG UINT32_C(0x38)

/* Port register array */
#define XHCI_PORTSC UINT32_C(0x400)
#define XHCI_PORTPMSC UINT32_C(0x404)
#define XHCI_PORTLI UINT32_C(0x408)
#define XHCI_PORTHLPMC UINT32_C(0x40C)

#define XHCI_PORT_REGISTER_OFFSET(port_number, register_offset)                \
  ((register_offset) + ((uint32_t)(port_number) * UINT32_C(0x10)))

/* USBCMD */
#define XHCI_CMD_RUN_STOP (UINT32_C(1) << 0)
#define XHCI_CMD_HCRST (UINT32_C(1) << 1)
#define XHCI_CMD_INTE (UINT32_C(1) << 2)
#define XHCI_CMD_HSEE (UINT32_C(1) << 3)
#define XHCI_CMD_LHCRST (UINT32_C(1) << 7)
#define XHCI_CMD_CSS (UINT32_C(1) << 8)
#define XHCI_CMD_CRS (UINT32_C(1) << 9)
#define XHCI_CMD_EWE (UINT32_C(1) << 10)
#define XHCI_CMD_EU3S (UINT32_C(1) << 11)

/* USBSTS */
#define XHCI_STS_HCH (UINT32_C(1) << 0)
#define XHCI_STS_HSE (UINT32_C(1) << 2)
#define XHCI_STS_EINT (UINT32_C(1) << 3)
#define XHCI_STS_PCD (UINT32_C(1) << 4)
#define XHCI_STS_SSS (UINT32_C(1) << 8)
#define XHCI_STS_RSS (UINT32_C(1) << 9)
#define XHCI_STS_SRE (UINT32_C(1) << 10)
#define XHCI_STS_CNR (UINT32_C(1) << 11)
#define XHCI_STS_HCE (UINT32_C(1) << 12)

/* PAGESIZE */
#define XHCI_PAGESIZE_MASK FIELD_MASK(16, 0)

/* DNCTRL */
#define XHCI_DNCTRL_ENABLE FIELD_MASK(16, 0)

/* CRCR */
#define XHCI_CRCR_RCS (UINT64_C(1) << 0)
#define XHCI_CRCR_CRR (UINT64_C(1) << 3)
#define XHCI_CRCR_CA (UINT64_C(1) << 4)
#define XHCI_CRCR_CRR_MASK UINT64_C(0xFFFFFFFFFFFFFFC0)

/* DCBAAP */
#define XHCI_DCBAAP_ADDRESS_MASK UINT64_C(0xFFFFFFFFFFFFFFC0)

/* CONFIG */
#define XHCI_CONFIG_MAX_SLOTS_ENABLED FIELD_MASK(8, 0)

/* PORTSC */
#define XHCI_PORTSC_CCS (UINT32_C(1) << 0)
#define XHCI_PORTSC_PED (UINT32_C(1) << 1)
#define XHCI_PORTSC_OCA (UINT32_C(1) << 3)
#define XHCI_PORTSC_PR (UINT32_C(1) << 4)
#define XHCI_PORTSC_PLS_MASK FIELD_MASK(4, 5)
#define XHCI_PORTSC_PP (UINT32_C(1) << 9)
#define XHCI_PORTSC_PORT_SPEED_MASK FIELD_MASK(4, 10)
#define XHCI_PORTSC_LWS (UINT32_C(1) << 16)
#define XHCI_PORTSC_CSC (UINT32_C(1) << 17) /* Write 1 to clear */
#define XHCI_PORTSC_PEC (UINT32_C(1) << 18) /* Write 1 to clear */
#define XHCI_PORTSC_WRC (UINT32_C(1) << 19) /* Write 1 to clear */
#define XHCI_PORTSC_OCC (UINT32_C(1) << 20) /* Write 1 to clear */
#define XHCI_PORTSC_PRC (UINT32_C(1) << 21) /* Write 1 to clear */
#define XHCI_PORTSC_PLC (UINT32_C(1) << 22) /* Write 1 to clear */
#define XHCI_PORTSC_CEC (UINT32_C(1) << 23) /* Write 1 to clear */
#define XHCI_PORTSC_CAS (UINT32_C(1) << 24)
#define XHCI_PORTSC_WCE (UINT32_C(1) << 25)
#define XHCI_PORTSC_WDE (UINT32_C(1) << 26)
#define XHCI_PORTSC_WOE (UINT32_C(1) << 27)
#define XHCI_PORTSC_DR (UINT32_C(1) << 30)
#define XHCI_PORTSC_WPR (UINT32_C(1) << 31)

/*
 * Runtime register space.
 *
 *     runtime_base = xhci_base + RTSOFF
 */
#define XHCI_IR0 UINT32_C(0x20)
#define XHCI_IR_IMAN UINT32_C(0x00)
#define XHCI_IR_IMOD UINT32_C(0x04)
#define XHCI_IR_ERSTSZ UINT32_C(0x08)
#define XHCI_IR_ERSTBA UINT32_C(0x10) /* 64-bit */
#define XHCI_IR_ERDP UINT32_C(0x18)   /* 64-bit */

/* IMAN */
#define XHCI_IMAN_IP (UINT32_C(1) << 0) /* Write 1 to clear */
#define XHCI_IMAN_IE (UINT32_C(1) << 1)

/* IMOD */
#define XHCI_IMOD_INTERVAL FIELD_MASK(16, 0)
#define XHCI_IMOD_COUNTER FIELD_MASK(16, 16)

/* ERSTSZ */
#define XHCI_ERSTSZ_SIZE FIELD_MASK(16, 0)

/* ERSTBA and ERDP */
#define XHCI_ERST_ADDRESS_MASK UINT64_C(0xFFFFFFFFFFFFFFC0)

/*
 * Doorbell registers
 *
 *     doorbell_base = xhci_base + DBOFF
 *
 * Doorbell 0 is the command ring doorbell.
 * Device doorbell n is used for slot n.
 */
#define XHCI_DOORBELL_OFFSET(doorbell_number)                                  \
  ((uint32_t)(doorbell_number) * UINT32_C(4))
#define XHCI_DB_TARGET_MASK FIELD_MASK(8, 0)
#define XHCI_DB_TASK_ID_MASK FIELD_MASK(16, 16)

/* HCSParams1 */
#define XHCI_HCSP1_MAX_SLOTS FIELD_MASK(8, 0)
#define XHCI_HCSP1_MAX_INTRS FIELD_MASK(11, 8)
#define XHCI_HCSP1_MAX_PORTS FIELD_MASK(8, 24)

/* HCCParams1 */
#define XHCI_HCC1_64BIT_ADDRESSING (UINT32_C(1) << 0)
#define XHCI_HCC1_BANDWIDTH_NEGOTIATION (UINT32_C(1) << 1)
#define XHCI_HCC1_CONTEXT_SIZE (UINT32_C(1) << 2)
#define XHCI_HCC1_PORT_POWER_CONTROL (UINT32_C(1) << 3)
#define XHCI_HCC1_PORT_INDICATORS (UINT32_C(1) << 4)
#define XHCI_HCC1_LIGHT_HOST_CONTROLLER (UINT32_C(1) << 5)
#define XHCI_HCC1_EXT_CAP_POINTER FIELD_MASK(16, 16)

struct xhci_trb;
struct xhci_ring;
struct xhci_event_ring_segment;
struct xhci_input_control_context;
struct xhci_slot_context;
struct xhci_endpoint_context;
struct xhci_device_context;
struct xhci_input_context;

/* Extended capability pointer is expressed in 32-bit DWORDs. */

int xhci_init(struct pci_device *dev);

#endif /* XHCI_H */
