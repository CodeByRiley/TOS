/* kernel/net/netmon.h , NIC counters and a frame-capture ring.
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
 * falls behind, old frames are overwritten rather than new ones dropped ,
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
 * Implementation: kernel/net/netmon.c. Shared syscall payloads live in
 * arch/syscall_abi.h.
 */
#ifndef NETMON_H
#define NETMON_H

#include <stddef.h>
#include <stdint.h>
#include <utilities/types.h>
#include <arch/syscall_abi.h>

#define NETMON_RING_FRAMES 64

#define NETMON_FRAME_BYTES NET_FRAME_BYTES
#define NETMON_CAPTURE_BATCH NET_CAPTURE_BATCH
#define NETMON_DIR_RX NET_DIR_RX
#define NETMON_DIR_TX NET_DIR_TX

/* ---------------- Driver-facing ---------------------------------------- */

/* Announce the interface. Safe to call again if the address changes. */
void netmon_bind(const u8 mac[6], const u8 ipv4[4]);

/* Link state, polled by the driver. speed_mbps is advisory. */
void netmon_set_link(int up, u32 speed_mbps);

/* Capture one frame. `length` is the wire length even when it exceeds
 * NETMON_FRAME_BYTES; only the first NETMON_FRAME_BYTES are stored. */
void netmon_record(int direction, const void *frame, u32 length);

/* ---------------- Syscall backends ------------------------------------- */

/* Both write straight into user memory, which the caller must have
 * validated first. Return 0 / the frame count, or -1. */
long netmon_read_stats(struct net_stats *out);

/* Copies frames from *cursor onward, advancing it past what was returned.
 * A cursor behind the ring jumps to the oldest frame still held. Compare
 * out[0].seq with the input cursor to detect the jump. */
long netmon_read_frames(u64 *cursor, struct net_frame *out,
                        long max);

#endif /* NETMON_H */
