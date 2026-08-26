/* kernel/net/arp.h — ARP cache, resolution, and the pending-frame queue.
 *
 * This is the piece that lets TOS speak first. The old driver-resident ARP
 * only ever answered: it read the requester's MAC out of the request it was
 * replying to, so it never needed a cache and could never originate a
 * packet. Everything above IP depends on the opposite — being handed a
 * destination IP and having to find the MAC.
 *
 * On a cache miss the frame is *queued*, not dropped. Dropping is simpler
 * and is what a first pass usually does, but it loses the first packet of
 * every new conversation, which then shows up as a mysterious one-second
 * stall at the start of each DNS lookup or connection and gets blamed on
 * the layer above. The queue is a fixed pool with no allocation, so it is
 * safe from the e1000 IRQ handler once that path lands; when the pool is
 * full the oldest pending frame is dropped rather than the newest, since a
 * frame that has already waited out an ARP timeout is the one least likely
 * to still matter.
 *
 * Implementation: kernel/net/arp.c.
 */
#ifndef NET_ARP_H
#define NET_ARP_H

#include <utilities/types.h>
#include <net/eth.h>
#include <net/inet.h>
#include <stdint.h>

#define ARP_HTYPE_ETHERNET 1U
#define ARP_OP_REQUEST 1U
#define ARP_OP_REPLY 2U

struct arp_ipv4 {
  uint16_t htype;
  uint16_t ptype;
  uint8_t hlen;
  uint8_t plen;
  uint16_t oper;
  uint8_t sha[ETH_ALEN];
  uint8_t spa[IPV4_ALEN];
  uint8_t tha[ETH_ALEN];
  uint8_t tpa[IPV4_ALEN];
} PACKED;

/* Called by eth_input for ETH_TYPE_ARP frames. */
void arp_input(const struct eth_hdr *eth, const uint8_t *payload, uint16_t len);

/* Cache lookup only. 0 and mac_out filled on a hit, -1 on a miss. Sends
 * nothing — use arp_send_or_queue() when there is a frame to deliver. */
int arp_lookup(const uint8_t ip[IPV4_ALEN], uint8_t mac_out[ETH_ALEN]);

/* Deliver `frame` (ETH_HDR_LEN of headroom + payload_len of payload) to
 * `ip`, resolving the MAC first.
 *   0  transmitted now
 *   1  queued behind an ARP request that has been sent
 *  -1  no interface, bad argument, or the frame could not be queued
 * A return of 1 is success from the caller's point of view: the frame goes
 * out when the reply arrives, or is dropped after ARP_MAX_RETRIES. */
int arp_send_or_queue(const uint8_t ip[IPV4_ALEN], uint8_t *frame,
                      uint16_t payload_len, uint16_t type);

/* Broadcast a request for `ip` without queueing anything. */
void arp_request(const uint8_t ip[IPV4_ALEN]);

/* Gratuitous ARP: a broadcast request for our own address. Every host on
 * the segment caches the binding without having to ask, and anything
 * already holding a stale entry for this address corrects it. Sent at
 * link-up, and by DHCP once a lease is taken.
 *
 * It is also the cheapest end-to-end proof that transmit works at all,
 * because it is the first frame TOS composes on its own initiative rather
 * than by turning a received one around. */
void arp_announce(void);

/* Resolve the gateway now rather than on the first packet that needs it.
 * Without this the first outbound datagram of the session -- a DHCP renew,
 * a DNS query -- is the one that pays for the resolution, sitting in the
 * pending queue for a round trip while the layer above it wonders why the
 * link is slow only once. Doing it at link-up moves that cost to boot,
 * where nothing is waiting. */
void arp_prime_gateway(void);

/* Expiry and retransmission. Driven from the driver poll task; cheap
 * enough to call every pass and does nothing when the cache is idle. */
void arp_tick(void);

/* Drop every entry — DHCP calls this when the address changes, because a
 * cache learned under the old address is answering for a different host. */
void arp_flush(void);

#endif /* NET_ARP_H */
