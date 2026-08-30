/* kernel/net/icmp.h , ICMP echo.
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

/* Payload carried by an outbound echo request. 32 bytes is what ping has
 * sent since time immemorial; matching it keeps captures comparable with
 * every other tool someone might put on the same wire. */
#define ICMP_PING_PAYLOAD 32

/* Concurrent pings in flight. One per caller, and there is exactly one
 * ping command today, so four is generous rather than a limit anyone will
 * reach -- but it is a fixed table, so it does have to be checked. */
#define ICMP_PING_SESSIONS 4

struct icmp_hdr {
  u8 type;
  u8 code;
  u16 checksum;
  u16 ident;
  u16 sequence;
} PACKED;

/* ---------------- Userspace ABI ----------------------------------------
 * Copied verbatim to and from user memory by SYS_NET_PING, so the layout
 * is ABI. The static assertion in icmp.c pins it. Mirrored as
 * struct net_ping in userspace/lib/syscall.h. */
struct net_ping_user {
  u8 dst[IPV4_ALEN];
  u16 ident;
  u16 seq;
  u32 timeout_ms;
  u32 rtt_ms; /* out: valid only when the call returns 0 */
};

/* Called by ipv4_input for IPPROTO_ICMP. `payload` is the ICMP message,
 * `len` its length; the IPv4 header is passed for the source address. */
void icmp_input(const struct ipv4_hdr *ip, const u8 *payload,
                u16 len);

/* Send one echo request to `dst` and wait for the reply that matches
 * `ident` and `seq`.
 *    0  reply seen; *rtt_ms holds the round trip
 *   -1  timed out
 *   -2  no interface, no route, or too many pings already in flight
 *
 * Waits by yielding, because the reply is delivered by the driver poll
 * task and can only arrive once this one stops running. That makes it
 * illegal to call from an interrupt handler or with a lock held, and it
 * is why the syscall wrapper copies its arguments in before calling. */
long icmp_ping(const u8 dst[IPV4_ALEN], u16 ident, u16 seq,
               u32 timeout_ms, u32 *rtt_ms_out);

#endif /* NET_ICMP_H */
