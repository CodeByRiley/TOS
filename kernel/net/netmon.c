/* kernel/net/netmon.c — implementation of the NIC capture ring.
 *
 * One global interface. TOS binds a single NIC today, and giving the ring
 * an index before there is a second card to put in it would be structure
 * without a user; netmon_bind() simply describes whichever driver called
 * it last.
 */
#include <devices/pit.h>
#include <net/netmon.h>
#include <sync/spinlock.h>
#include <utilities/string.h>

_Static_assert(sizeof(struct netmon_frame_user) == 160,
               "netmon_frame_user layout is userspace ABI");
_Static_assert(sizeof(struct netmon_stats_user) == 80,
               "netmon_stats_user layout is userspace ABI");
_Static_assert(offsetof(struct netmon_frame_user, data) == 32,
               "netmon_frame_user.data offset is userspace ABI");

static struct {
  spinlock_t lock;
  struct netmon_frame_user ring[NETMON_RING_FRAMES];
  uint64_t seq_next;
  uint64_t rx_frames, rx_bytes;
  uint64_t tx_frames, tx_bytes;
  uint8_t mac[6];
  uint8_t ipv4[4];
  uint32_t link_up;
  uint32_t speed_mbps;
  uint32_t present;
} monitor = {.lock = SPINLOCK_INIT};

/* The driver's poll callback runs in a kernel task rather than an
 * interrupt handler today, but an e1000 IRQ path is the obvious next step
 * and would call straight into netmon_record(). Taking the IRQ-safe
 * variant now costs a pushfq and makes that change a non-event. */

void netmon_bind(const uint8_t mac[6], const uint8_t ipv4[4]) {
  uint64_t flags = spin_lock_irqsave(&monitor.lock);
  if (mac)
    memcpy(monitor.mac, mac, sizeof(monitor.mac));
  if (ipv4)
    memcpy(monitor.ipv4, ipv4, sizeof(monitor.ipv4));
  monitor.present = 1;
  spin_unlock_irqrestore(&monitor.lock, flags);
}

void netmon_set_link(int up, uint32_t speed_mbps) {
  uint64_t flags = spin_lock_irqsave(&monitor.lock);
  monitor.link_up = up ? 1u : 0u;
  monitor.speed_mbps = speed_mbps;
  spin_unlock_irqrestore(&monitor.lock, flags);
}

void netmon_record(int direction, const void *frame, uint32_t length) {
  if (!frame || length == 0)
    return;

  uint32_t captured = length;
  if (captured > NETMON_FRAME_BYTES)
    captured = NETMON_FRAME_BYTES;

  uint64_t flags = spin_lock_irqsave(&monitor.lock);

  struct netmon_frame_user *slot =
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

  spin_unlock_irqrestore(&monitor.lock, flags);
}

long netmon_read_stats(struct netmon_stats_user *out) {
  if (!out)
    return -1;

  uint64_t flags = spin_lock_irqsave(&monitor.lock);
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

long netmon_read_frames(uint64_t *cursor, struct netmon_frame_user *out,
                        long max) {
  if (!cursor || !out || max <= 0)
    return -1;
  if (max > NETMON_CAPTURE_BATCH)
    max = NETMON_CAPTURE_BATCH;

  uint64_t flags = spin_lock_irqsave(&monitor.lock);

  uint64_t oldest = monitor.seq_next > NETMON_RING_FRAMES
                        ? monitor.seq_next - NETMON_RING_FRAMES
                        : 0;
  uint64_t at = *cursor;
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
