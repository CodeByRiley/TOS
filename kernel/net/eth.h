/* kernel/net/eth.h , Ethernet framing and receive demultiplexing.
 *
 * The NIC runs promiscuous (e1000 sets RCTL_UPE|RCTL_MPE) so netmon can
 * capture the whole segment. That is the right setting for a monitor and
 * the wrong one for a protocol stack, so the address filter lives here
 * instead: netmon records what the driver saw, eth_input decides what the
 * protocols are allowed to answer. Without that split, ARP and ICMP would
 * be reached by frames addressed to other hosts and would be relying on
 * their own destination-address checks as the only line of defence.
 *
 * Transmit comes in two shapes because prepending a header to a payload
 * otherwise costs a second full-frame buffer. eth_output_framed() writes
 * into ETH_HDR_LEN bytes of headroom the caller already reserved, so
 * ipv4_output() builds one 1514-byte frame instead of two; eth_output()
 * copies, and is for small callers like ARP where the extra buffer is
 * nothing.
 *
 * Implementation: kernel/net/eth.c.
 */
#ifndef NET_ETH_H
#define NET_ETH_H

#include <utilities/types.h>
#include <net/inet.h>
#include <stdint.h>

#define ETH_ALEN 6
#define ETH_HDR_LEN 14
/* Payload only; the FCS is the NIC's business and RCTL_SECRC strips it. */
#define ETH_FRAME_MAX 1514
#define ETH_FRAME_MIN 60

#define ETH_TYPE_IPV4 0x0800U
#define ETH_TYPE_ARP 0x0806U

struct eth_hdr {
  u8 dst[ETH_ALEN];
  u8 src[ETH_ALEN];
  u16 type; /* wire order */
} PACKED;

extern const u8 eth_broadcast[ETH_ALEN];

/* Entry point from a NIC driver, once per received frame. */
void eth_input(const u8 *frame, u16 len);

/* Fill in the header of a frame whose payload already sits at
 * frame + ETH_HDR_LEN, then transmit. Returns 0 or -1. */
int eth_output_framed(u8 *frame, const u8 dst[ETH_ALEN],
                      u16 type, u16 payload_len);

/* Copy `payload` behind a fresh header and transmit. Returns 0 or -1. */
int eth_output(const u8 dst[ETH_ALEN], u16 type, const void *payload,
               u16 payload_len);

#endif /* NET_ETH_H */
