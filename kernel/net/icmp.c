/* kernel/net/icmp.c — see icmp.h. */
#include <net/icmp.h>
#include <net/ipv4.h>
#include <net/netif.h>
#include <utilities/string.h>

void icmp_input(const struct ipv4_hdr *ip, const uint8_t *payload,
                uint16_t len) {
  if (!ip || !payload || len < ICMP_HDR_LEN || len > IPV4_PAYLOAD_MAX)
    return;

  const struct icmp_hdr *icmp = (const struct icmp_hdr *)payload;
  if (icmp->type != ICMP_ECHO_REQUEST || icmp->code != 0)
    return;

  /* The driver version this replaces skipped the inbound checksum and
   * echoed whatever arrived. Verifying it costs one pass over a packet we
   * are about to copy anyway, and without it a corrupted request comes
   * back as a well-formed reply that makes the link look healthy. */
  if (inet_checksum(payload, len) != 0)
    return;

  uint8_t frame[ETH_FRAME_MAX];
  uint8_t *reply_payload = frame + IPV4_HEADROOM;
  memcpy(reply_payload, payload, len);

  /* Identifier, sequence, and body are echoed back unchanged — that is
   * what makes the reply match the request the sender is waiting on. */
  struct icmp_hdr *reply = (struct icmp_hdr *)reply_payload;
  reply->type = ICMP_ECHO_REPLY;
  reply->checksum = 0;
  reply->checksum = inet_checksum(reply_payload, len);

  /* Replies go back through ipv4_output like anything else, rather than
   * turning the received frame around in place. That costs an ARP lookup
   * the old path did not need, and buys the guarantee that a bug in the
   * normal output path cannot hide behind a working ping. */
  ipv4_output_framed(frame, ip->src, IPPROTO_ICMP, len);
}
