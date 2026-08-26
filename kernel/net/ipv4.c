/* kernel/net/ipv4.c , see ipv4.h. */
#include "net/udp.h"
#include "utilities/log.h"
#include <net/arp.h>
#include <net/eth.h>
#include <net/icmp.h>
#include <net/ipv4.h>
#include <net/netif.h>
#include <utilities/string.h>

/* Wraps freely. The ID only has to distinguish packets that could be in
 * flight at the same time, and nothing here fragments. */
static uint16_t ipv4_next_id;

/* Where to send a packet for `dst`: the host itself when it is on-link,
 * otherwise the gateway. Returns -1 when neither applies, which is the
 * honest answer for an off-link destination with no gateway configured. */
static int ipv4_next_hop(const struct netif *nif, const uint8_t *dst,
                         uint8_t hop[IPV4_ALEN]) {
  if (ipv4_same_subnet(dst, nif->ipv4, nif->netmask)) {
    memcpy(hop, dst, IPV4_ALEN);
    return 0;
  }
  if (ipv4_addr_is_zero(nif->gateway))
    return -1;
  memcpy(hop, nif->gateway, IPV4_ALEN);
  return 0;
}

static int ipv4_dst_is_broadcast(const struct netif *nif, const uint8_t *dst) {
  static const uint8_t all_ones[IPV4_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF};
  return ipv4_addr_equal(dst, all_ones) ||
         ipv4_is_broadcast(dst, nif->ipv4, nif->netmask);
}

int ipv4_output_framed(uint8_t *frame, const uint8_t dst[IPV4_ALEN],
                       uint8_t protocol, uint16_t payload_len) {
  struct netif *nif = netif_get();
  if (!nif || !frame || !dst || payload_len > IPV4_PAYLOAD_MAX)
    return -1;

  uint16_t total_len = (uint16_t)(IPV4_HDR_LEN + payload_len);

  struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + ETH_HDR_LEN);
  ip->version_ihl = 0x45; /* IPv4, 20-byte header, no options */
  ip->tos = 0;
  ip->total_length = to_be16(total_len);
  ip->id = to_be16(ipv4_next_id++);
  ip->flags_fragment = 0;
  ip->ttl = IPV4_DEFAULT_TTL;
  ip->protocol = protocol;
  memcpy(ip->src, nif->ipv4, IPV4_ALEN);
  memcpy(ip->dst, dst, IPV4_ALEN);
  ip->checksum = 0;
  ip->checksum = inet_checksum(ip, IPV4_HDR_LEN);

  /* Broadcast skips ARP: there is nothing to resolve, and asking would
   * stall DHCP behind a request only the DHCP server could answer. */
  if (ipv4_dst_is_broadcast(nif, dst))
    return eth_output_framed(frame, eth_broadcast, ETH_TYPE_IPV4, total_len);

  // uint8_t qemu_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  // return eth_output_framed(frame, qemu_mac, ETH_TYPE_IPV4, total_len);

  uint8_t hop[IPV4_ALEN];
  if (ipv4_next_hop(nif, dst, hop) != 0)
    return -1;

  return arp_send_or_queue(hop, frame, total_len, ETH_TYPE_IPV4);
}

int ipv4_output(const uint8_t dst[IPV4_ALEN], uint8_t protocol,
                const void *payload, uint16_t payload_len) {
  if (payload_len > IPV4_PAYLOAD_MAX)
    return -1;

  uint8_t frame[ETH_FRAME_MAX];
  if (payload && payload_len)
    memcpy(frame + IPV4_HEADROOM, payload, payload_len);

  return ipv4_output_framed(frame, dst, protocol, payload_len);
}

void ipv4_input(const struct eth_hdr *eth, const uint8_t *packet,
                uint16_t len) {
  struct netif *nif = netif_get();
  if (!nif || !packet || len < IPV4_HDR_LEN)
    return;
  (void)eth;

  const struct ipv4_hdr *ip = (const struct ipv4_hdr *)packet;
  if ((ip->version_ihl >> 4) != 4)
    return;

  uint16_t ihl = (uint16_t)((ip->version_ihl & 0x0F) * 4);
  uint16_t total_len = from_be16(ip->total_length);

  /* total_len may be shorter than len , Ethernet pads to 60 bytes, so a
   * small packet always arrives with trailing filler. Trust the header,
   * but only after checking it fits inside what was actually received. */
  if (ihl < IPV4_HDR_LEN || total_len < ihl || total_len > len)
    return;

  if (inet_checksum(ip, ihl) != 0)
    return;

  /* MF set, or a non-zero offset: a fragment. Dropped rather than treated
   * as a whole packet, which is what reading past the header would do. */
  if (from_be16(ip->flags_fragment) & 0x3FFF)
    return;

  log_write_fmt(KERNEL, LOG_DEBUG,
                "RX IP packet for %d.%d.%d.%d. My IP is %d.%d.%d.%d\n",
                ip->dst[0], ip->dst[1], ip->dst[2], ip->dst[3], nif->ipv4[0],
                nif->ipv4[1], nif->ipv4[2], nif->ipv4[3]);

  if (!ipv4_addr_equal(ip->dst, nif->ipv4) &&
      !ipv4_dst_is_broadcast(nif, ip->dst))
    return;

  const uint8_t *payload = packet + ihl;
  uint16_t payload_len = (uint16_t)(total_len - ihl);

  switch (ip->protocol) {
  case IPPROTO_ICMP:
    icmp_input(ip, payload, payload_len);
    break;
  case IPPROTO_UDP: {
    log_write("net: UDP packet received", KERNEL, LOG_INFO);

    const struct udp_header *udp = (const struct udp_header *)payload;
    const uint8_t *udp_payload = payload + UDP_HEADER_SIZE;
    uint16_t udp_payload_len = payload_len - UDP_HEADER_SIZE;

    struct ipv4_addr src = *(struct ipv4_addr *)ip->src;
    struct ipv4_addr dst = *(struct ipv4_addr *)ip->dst;

    socket_handle_incoming(src,                 // src_ip
                           udp->src_port,       // src_port
                           dst,                 // dest_ip
                           udp->dst_port,       // dest_port
                           IPPROTO_UDP,         // protocol
                           (void *)udp_payload, // payload
                           udp_payload_len      // length
    );
    break;
  }
  case IPPROTO_TCP:
    log_write("net: UDP packet received", KERNEL, LOG_INFO);
    break;
  default:
    /* UDP and TCP land here once the socket layer is honest. */
    break;
  }
}
