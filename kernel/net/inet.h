/* kernel/net/inet.h , byte order, checksums, and shared protocol numbers.
 *
 * The wire is big-endian and x86 is little-endian, so every multi-byte
 * header field needs a swap somewhere. Keeping the swap in one header
 * instead of one copy per protocol file is what prevents the classic bug
 * where a field ends up swapped twice, or not at all, on a path that only
 * fires for one packet shape.
 *
 * Every header struct in kernel/net stores its fields in *wire* order.
 * Read them through from_be16/from_be32 and write them through
 * to_be16/to_be32; never compare a raw struct field against a host
 * constant.
 */
#ifndef NET_INET_H
#define NET_INET_H

#include <utilities/types.h>
#include <stdint.h>

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1U
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6U
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17U
#endif

#define IPV4_ALEN 4

SINLINE u16 bswap16(u16 value) {
  return (u16)((value << 8) | (value >> 8));
}

SINLINE u32 bswap32(u32 value) {
  return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
         ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

SINLINE u16 to_be16(u16 value) { return bswap16(value); }
SINLINE u16 from_be16(u16 value) { return bswap16(value); }
SINLINE u32 to_be32(u32 value) { return bswap32(value); }
SINLINE u32 from_be32(u32 value) { return bswap32(value); }

/* One's complement sum of `len` bytes, returned in wire order , assign it
 * straight into a header's checksum field without a further swap. The
 * accumulator reads bytes pairwise rather than casting to u16*
 * because callers hand it unaligned pointers into receive buffers. */
u16 inet_checksum(const void *data, u32 len);

SINLINE int ipv4_addr_equal(const u8 *a, const u8 *b) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

SINLINE int ipv4_addr_is_zero(const u8 *a) {
  return (a[0] | a[1] | a[2] | a[3]) == 0;
}

/* Broadcast for `ip` under `mask`: every host bit set. */
SINLINE int ipv4_is_broadcast(const u8 *addr, const u8 *ip,
                                    const u8 *mask) {
  for (int i = 0; i < IPV4_ALEN; i++) {
    if ((u8)(addr[i] | mask[i]) != 0xFFu)
      return 0;
    if ((u8)(ip[i] & mask[i]) != (u8)(addr[i] & mask[i]))
      return 0;
  }
  return 1;
}

/* Same subnet as `ip` under `mask` , decides ARP-for-the-host versus
 * ARP-for-the-gateway in ipv4_output(). */
SINLINE int ipv4_same_subnet(const u8 *a, const u8 *b,
                                   const u8 *mask) {
  for (int i = 0; i < IPV4_ALEN; i++) {
    if ((u8)(a[i] & mask[i]) != (u8)(b[i] & mask[i]))
      return 0;
  }
  return 1;
}

#endif /* NET_INET_H */
