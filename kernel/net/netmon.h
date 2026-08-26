/* kernel/net/netmon.h — NIC counters and a frame-capture ring.
 *
 * A NIC driver calls netmon_record() for every frame it hands up or puts
 * on the wire; the capture lands in a fixed ring that userspace drains
 * through SYS_NET_CAPTURE. The point is to make the link observable
 * before there is a protocol stack worth speaking to: with only ARP and
 * ICMP answered in the driver, `ping` from the host is the whole test
 * surface, and reading it off the serial log means the interesting bytes
 * scroll past mixed into unrelated kernel output.
 *
 * The ring never blocks a driver and never allocates. When userspace
 * falls behind, old frames are overwritten rather than new ones dropped —
 * a monitor that stalls should lose history, not stop the NIC. Readers
 * detect the loss themselves: every frame carries the sequence number it
 * was captured with, so a cursor that comes back further along than it
 * asked for names exactly how many frames went missing.
 *
 * Only the first NETMON_FRAME_BYTES of a frame are kept. That is enough
 * for Ethernet + IPv4 + a TCP or UDP header and some payload, which is
 * what identifying traffic actually needs; full-length capture would cost
 * 24x the memory to show payload nobody is reading yet.
 *
 * Implementation: kernel/net/netmon.c. Userspace mirror of these two
 * structs: struct net_stats / struct net_frame in userspace/lib/syscall.h.
 */
#ifndef NETMON_H
#define NETMON_H

#include <stddef.h>
#include <stdint.h>

#define NETMON_RING_FRAMES 64
#define NETMON_FRAME_BYTES 128

/* Largest batch one SYS_NET_CAPTURE call will return. Bounds how long the
 * ring lock is held while copying into user memory. */
#define NETMON_CAPTURE_BATCH 16

#define NETMON_DIR_RX 0
#define NETMON_DIR_TX 1

/* ---------------- Userspace ABI ----------------------------------------
 * Both structs are copied verbatim into user memory, so their layout is
 * ABI. The static assertions in netmon.c pin it. */

struct netmon_frame_user {
  uint64_t seq;       /* capture sequence, unique and monotonic     */
  uint64_t ticks;     /* scheduler ticks when captured              */
  uint32_t length;    /* frame length on the wire                   */
  uint32_t captured;  /* bytes actually in data[], <= length        */
  uint32_t direction; /* NETMON_DIR_RX or NETMON_DIR_TX             */
  uint32_t reserved;
  uint8_t  data[NETMON_FRAME_BYTES];
};

struct netmon_stats_user {
  uint64_t rx_frames;
  uint64_t rx_bytes;
  uint64_t tx_frames;
  uint64_t tx_bytes;
  uint64_t seq_next;    /* sequence the next captured frame will get */
  uint64_t seq_oldest;  /* oldest sequence still held in the ring    */
  uint8_t  mac[6];
  uint8_t  ipv4[4];
  uint32_t link_up;
  uint32_t speed_mbps;
  uint32_t present;     /* 0 when no driver has called netmon_bind() */
  uint32_t ring_frames; /* NETMON_RING_FRAMES, so callers need not
                         * hardcode the capture depth                */
};

/* ---------------- Driver-facing ---------------------------------------- */

/* Announce the interface. Safe to call again if the address changes. */
void netmon_bind(const uint8_t mac[6], const uint8_t ipv4[4]);

/* Link state, polled by the driver. speed_mbps is advisory. */
void netmon_set_link(int up, uint32_t speed_mbps);

/* Capture one frame. `length` is the wire length even when it exceeds
 * NETMON_FRAME_BYTES; only the first NETMON_FRAME_BYTES are stored. */
void netmon_record(int direction, const void *frame, uint32_t length);

/* ---------------- Syscall backends ------------------------------------- */

/* Both write straight into user memory, which the caller must have
 * validated first. Return 0 / the frame count, or -1. */
long netmon_read_stats(struct netmon_stats_user *out);

/* Copies frames from *cursor onward, advancing it past what was returned.
 * A cursor behind the ring jumps to the oldest frame still held. Compare
 * out[0].seq with the input cursor to detect the jump. */
long netmon_read_frames(uint64_t *cursor, struct netmon_frame_user *out,
                        long max);

#endif /* NETMON_H */
