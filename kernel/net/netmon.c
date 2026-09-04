/* kernel/net/netmon.c , implementation of the NIC capture ring.
 *
 * One global interface. TOS binds a single NIC today, and giving the ring
 * an index before there is a second card to put in it would be structure
 * without a user; netmon_bind() simply describes whichever driver called
 * it last.
 */
#include "utilities/log.h"
#include <devices/pit.h>
#include <net/netmon.h>
#include <sync/spinlock.h>
#include <utilities/string.h>

static struct {
  struct spinlock lock;
  struct net_frame ring[NETMON_RING_FRAMES];
  u64 seq_next;
  u64 rx_frames, rx_bytes;
  u64 tx_frames, tx_bytes;
  u8 mac[6];
  u8 ipv4[4];
  u32 link_up;
  u32 speed_mbps;
  u32 present;
} monitor = {.lock = SPINLOCK_INIT};

/* The driver's poll callback runs in a kernel task rather than an
 * interrupt handler today, but an e1000 IRQ path is the obvious next step
 * and would call straight into netmon_record(). Taking the IRQ-safe
 * variant now costs a pushfq and makes that change a non-event. */

void netmon_bind(const u8 mac[6], const u8 ipv4[4]) {
  u64 flags = spin_lock_irqsave(&monitor.lock);
  if (mac)
    memcpy(monitor.mac, mac, sizeof(monitor.mac));
  if (ipv4)
    memcpy(monitor.ipv4, ipv4, sizeof(monitor.ipv4));
  monitor.present = 1;
  spin_unlock_irqrestore(&monitor.lock, flags);
}

void netmon_set_link(int up, u32 speed_mbps) {
  u64 flags = spin_lock_irqsave(&monitor.lock);
  monitor.link_up = up ? 1u : 0u;
  monitor.speed_mbps = speed_mbps;
  spin_unlock_irqrestore(&monitor.lock, flags);
}

void netmon_record(int direction, const void *frame, u32 length) {
  if (!frame || length == 0)
    return;

  u32 captured = length;
  if (captured > NETMON_FRAME_BYTES)
    captured = NETMON_FRAME_BYTES;

  u64 flags = spin_lock_irqsave(&monitor.lock);

  struct net_frame *slot =
      &monitor.ring[monitor.seq_next % NETMON_RING_FRAMES];
  slot->seq = monitor.seq_next;
  slot->ticks = pit_ticks();
  slot->length = length;
  slot->captured = captured;
  slot->direction = (direction == NETMON_DIR_TX) ? NETMON_DIR_TX
                                                 : NETMON_DIR_RX;
  slot->reserved = 0;
  memcpy(slot->data, frame, captured);
  /* Zero the tail so a short frame does not show the previous occupant's
   * bytes in a hex dump. */
  if (captured < NETMON_FRAME_BYTES)
    memset(slot->data + captured, 0, NETMON_FRAME_BYTES - captured);

  monitor.seq_next++;

  if (slot->direction == NETMON_DIR_TX) {
    monitor.tx_frames++;
    monitor.tx_bytes += length;
  } else {
    monitor.rx_frames++;
    monitor.rx_bytes += length;
  }

  log_write_fmt(KERNEL, LOG_INFO,"netmon: captured frame %d, %d bytes, %s",
               slot->seq, slot->length,
               slot->direction == NETMON_DIR_TX ? "TX" : "RX");

  spin_unlock_irqrestore(&monitor.lock, flags);
}

long netmon_read_stats(struct net_stats *out) {
  if (!out)
    return -1;

  u64 flags = spin_lock_irqsave(&monitor.lock);
  out->rx_frames = monitor.rx_frames;
  out->rx_bytes = monitor.rx_bytes;
  out->tx_frames = monitor.tx_frames;
  out->tx_bytes = monitor.tx_bytes;
  out->seq_next = monitor.seq_next;
  out->seq_oldest = monitor.seq_next > NETMON_RING_FRAMES
                        ? monitor.seq_next - NETMON_RING_FRAMES
                        : 0;
  memcpy(out->mac, monitor.mac, sizeof(out->mac));
  memcpy(out->ipv4, monitor.ipv4, sizeof(out->ipv4));
  out->link_up = monitor.link_up;
  out->speed_mbps = monitor.speed_mbps;
  out->present = monitor.present;
  out->ring_frames = NETMON_RING_FRAMES;
  spin_unlock_irqrestore(&monitor.lock, flags);
  return 0;
}

long netmon_read_frames(u64 *cursor, struct net_frame *out,
                        long max) {
  if (!cursor || !out || max <= 0)
    return -1;
  if (max > NETMON_CAPTURE_BATCH)
    max = NETMON_CAPTURE_BATCH;

  u64 flags = spin_lock_irqsave(&monitor.lock);

  u64 oldest = monitor.seq_next > NETMON_RING_FRAMES
                        ? monitor.seq_next - NETMON_RING_FRAMES
                        : 0;
  u64 at = *cursor;
  /* Behind the ring: skip to what still exists. The caller sees the jump
   * in out[0].seq and can report the gap. */
  if (at < oldest)
    at = oldest;

  long count = 0;
  while (count < max && at < monitor.seq_next) {
    out[count] = monitor.ring[at % NETMON_RING_FRAMES];
    count++;
    at++;
  }
  *cursor = at;

  spin_unlock_irqrestore(&monitor.lock, flags);
  return count;
}
