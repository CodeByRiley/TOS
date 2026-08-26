/* kernel/net/icmp.h — ICMP echo.
 *
 * Echo reply is the whole of it for now, and it is not a toy: `ping` from
 * the host is still the cheapest end-to-end proof that the RX ring, the
 * address filter, ARP resolution, the IPv4 checksum, and the TX ring are
 * all correct at once. Keeping it here rather than in the driver means it
 * exercises the same output path everything above it will use, so a break
 * in ipv4_output shows up as a failed ping instead of hiding until UDP.
 *
 * Implementation: kernel/net/icmp.c.
 */
#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <utilities/types.h>
#include <net/ipv4.h>
#include <stdint.h>

#define ICMP_HDR_LEN 8
#define ICMP_ECHO_REPLY 0U
#define ICMP_ECHO_REQUEST 8U

struct icmp_hdr {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t ident;
  uint16_t sequence;
} PACKED;

/* Called by ipv4_input for IPPROTO_ICMP. `payload` is the ICMP message,
 * `len` its length; the IPv4 header is passed for the source address. */
void icmp_input(const struct ipv4_hdr *ip, const uint8_t *payload,
                uint16_t len);

#endif /* NET_ICMP_H */
