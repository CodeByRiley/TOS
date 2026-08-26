/* kernel/net/eth.c , see eth.h. */
#include "utilities/log.h"
#include <net/arp.h>
#include <net/eth.h>
#include <net/ipv4.h>
#include <net/netif.h>
#include <utilities/string.h>

const uint8_t eth_broadcast[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Bit 0 of the first octet marks a group address, which covers broadcast
 * as well as multicast. Accepting the whole group range rather than only
 * the exact broadcast address is what lets ARP-probing hosts and, later,
 * mDNS/DHCP reach us without another special case here. */
static int eth_addressed_to_us(const struct netif *nif, const uint8_t *dst) {
  if (dst[0] & 0x01)
    return 1;
  return memcmp(dst, nif->mac, ETH_ALEN) == 0;
}

void eth_input(const uint8_t *frame, uint16_t len) {
  log_write("eth_input: called", KERNEL, LOG_DEBUG);
  struct netif *nif = netif_get();
  if (!nif || !frame || len < ETH_HDR_LEN || len > ETH_FRAME_MAX)
    return;

  log_write("eth_input: frame is valid", KERNEL, LOG_DEBUG);

  const struct eth_hdr *eth = (const struct eth_hdr *)frame;
  if (!eth_addressed_to_us(nif, eth->dst))
    return;

  log_write("eth_input: frame is addressed to us", KERNEL, LOG_DEBUG);

  const uint8_t *payload = frame + ETH_HDR_LEN;
  uint16_t payload_len = (uint16_t)(len - ETH_HDR_LEN);

  switch (from_be16(eth->type)) {
  case ETH_TYPE_ARP:
    log_write("eth_input: ARP frame", KERNEL, LOG_DEBUG);
    arp_input(eth, payload, payload_len);
    break;
  case ETH_TYPE_IPV4:
    log_write("eth_input: IPv4 frame", KERNEL, LOG_DEBUG);
    ipv4_input(eth, payload, payload_len);
    break;
  default:
    /* Ignore unknown types */
    log_write_fmt(KERNEL, LOG_DEBUG,
                  "eth_input: unknown type %04x, frame, len",
                  from_be16(eth->type), frame, len);
    break;
  }
}

int eth_output_framed(uint8_t *frame, const uint8_t dst[ETH_ALEN],
                      uint16_t type, uint16_t payload_len) {
  struct netif *nif = netif_get();
  if (!nif || !frame || !dst || payload_len > nif->mtu)
    return -1;

  struct eth_hdr *eth = (struct eth_hdr *)frame;
  memcpy(eth->dst, dst, ETH_ALEN);
  memcpy(eth->src, nif->mac, ETH_ALEN);
  eth->type = to_be16(type);

  return netif_tx(frame, (uint16_t)(ETH_HDR_LEN + payload_len));
}

int eth_output(const uint8_t dst[ETH_ALEN], uint16_t type, const void *payload,
               uint16_t payload_len) {
  if (payload_len > ETH_FRAME_MAX - ETH_HDR_LEN)
    return -1;

  uint8_t frame[ETH_FRAME_MAX];
  if (payload && payload_len)
    memcpy(frame + ETH_HDR_LEN, payload, payload_len);

  return eth_output_framed(frame, dst, type, payload_len);
}
