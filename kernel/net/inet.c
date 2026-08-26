/* kernel/net/inet.c — see inet.h. */
#include <net/inet.h>
#include <utilities/string.h>

/* Sums 16-bit words in the order they sit in memory rather than converting
 * each to host order first. The one's complement sum has the property that
 * doing it this way yields a result already in wire order, so callers store
 * it straight into the header with no swap — and the swap that a
 * convert-then-sum version needs is the single most common place to get an
 * IP checksum wrong.
 *
 * memcpy for the word read because callers pass unaligned pointers into
 * receive buffers, where a uint16_t* cast would be undefined behaviour that
 * happens to work until the compiler vectorises the loop.
 *
 * The trailing odd byte is added as-is: on little-endian x86 the padded
 * word (byte, 0x00) has that byte in the low position. This file is
 * x86_64-only, like the rest of the kernel. */
uint16_t inet_checksum(const void *data, uint32_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t sum = 0;

  while (len > 1) {
    uint16_t word;
    memcpy(&word, bytes, sizeof(word));
    sum += word;
    bytes += 2;
    len -= 2;
  }

  if (len)
    sum += (uint16_t)bytes[0];

  while (sum >> 16)
    sum = (sum & 0xFFFFu) + (sum >> 16);

  return (uint16_t)~sum;
}
