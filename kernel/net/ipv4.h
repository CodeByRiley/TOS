/* kernel/net/ipv4.h , IPv4 input demultiplexing and output.
 *
 * Output is the new capability here. The driver-resident ICMP this replaces
 * could only turn a received packet around in place, so it never had to
 * choose a next hop or find a MAC; anything that originates a packet does.
 * ipv4_output() picks the next hop (on-link destination, or the gateway)
 * and hands the frame to arp_send_or_queue, which resolves it.
 *
 * Callers build into IPV4_HEADROOM bytes of headroom so one buffer carries
 * the frame all the way down: the payload is written at
 * frame + IPV4_HEADROOM, the IPv4 header is filled in behind it, and the
 * Ethernet header behind that. Prepending by copying instead would mean a
 * second 1514-byte buffer at every layer, and this runs on a kernel stack.
 *
 * No fragmentation, in or out. Fragmented inbound packets are dropped
 * rather than half-processed, and outbound payloads over the MTU are
 * refused at the call site where the caller can still do something about
 * it. Reassembly is worth writing when something needs it; today nothing
 * does, and a silently-wrong reassembler is worse than an honest refusal.
 *
 * Implementation: kernel/net/ipv4.c.
 */
#ifndef NET_IPV4_H
#define NET_IPV4_H

#include <utilities/types.h>
#include <net/eth.h>
#include <net/inet.h>
#include <stdint.h>

#define IPV4_HDR_LEN 20
#define IPV4_HEADROOM (ETH_HDR_LEN + IPV4_HDR_LEN)
#define IPV4_PAYLOAD_MAX (ETH_FRAME_MAX - IPV4_HEADROOM)
#define IPV4_DEFAULT_TTL 64

struct ipv4_hdr {
  u8 version_ihl;
  u8 tos;
  u16 total_length;
  u16 id;
  u16 flags_fragment;
  u8 ttl;
  u8 protocol;
  u16 checksum;
  u8 src[IPV4_ALEN];
  u8 dst[IPV4_ALEN];
} PACKED;

/* Called by eth_input for ETH_TYPE_IPV4 frames. */
void ipv4_input(const struct eth_hdr *eth, const u8 *packet, u16 len);

/* Fill in the IPv4 header of a frame whose payload already sits at
 * frame + IPV4_HEADROOM, then resolve and transmit.
 *   0  transmitted
 *   1  queued behind an ARP request
 *  -1  no interface, no route, or payload over the MTU
 * `frame` must be writable for at least IPV4_HEADROOM + payload_len. */
int ipv4_output_framed(u8 *frame, const u8 dst[IPV4_ALEN],
                       u8 protocol, u16 payload_len);

/* Copying convenience wrapper for callers without headroom. */
int ipv4_output(const u8 dst[IPV4_ALEN], u8 protocol,
                const void *payload, u16 payload_len);

#endif /* NET_IPV4_H */
