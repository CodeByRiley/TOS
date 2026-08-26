/* kernel/net/icmp.c - see icmp.h. */
#include <devices/pit.h>
#include <net/icmp.h>
#include <net/ipv4.h>
#include <net/netif.h>
#include <sched/sched.h>
#include <sync/spinlock.h>
#include <utilities/string.h>

_Static_assert(sizeof(struct net_ping_user) == 16,
               "net_ping_user layout is userspace ABI");

/* A ping in flight. The requesting task fills this in and then yields; the
 * driver poll task writes `replied` and `reply_ticks` when the matching
 * echo reply arrives. Both sides go through the lock, because a plain
 * volatile flag would still leave the tick read racing the write it is
 * paired with. */
struct icmp_session {
  int in_use;
  int replied;
  uint16_t ident;
  uint16_t seq;
  uint8_t dst[IPV4_ALEN];
  uint64_t sent_ticks;
  uint64_t reply_ticks;
};

static struct {
  struct spinlock lock;
  struct icmp_session slot[ICMP_PING_SESSIONS];
} pings = {.lock = SPINLOCK_INIT};

static uint32_t icmp_ticks_to_ms(uint64_t ticks) {
  uint32_t hz = pit_get_freq();
  if (hz == 0)
    return 0;
  return (uint32_t)((ticks * 1000u) / hz);
}

/* ---------------- receive ---------------------------------------------- */

static void icmp_echo_reply_input(const struct ipv4_hdr *ip,
                                  const struct icmp_hdr *icmp) {
  uint16_t ident = from_be16(icmp->ident);
  uint16_t seq = from_be16(icmp->sequence);

  uint64_t flags = spin_lock_irqsave(&pings.lock);
  for (int i = 0; i < ICMP_PING_SESSIONS; i++) {
    struct icmp_session *s = &pings.slot[i];
    if (!s->in_use || s->replied)
      continue;
    /* Source address as well as ident/seq: a reply from the wrong host
     * carrying a guessed identifier would otherwise stop the wait early
     * and report a round trip that never happened. */
    if (s->ident != ident || s->seq != seq ||
        !ipv4_addr_equal(s->dst, ip->src))
      continue;

    s->reply_ticks = pit_ticks();
    s->replied = 1;
    break;
  }
  spin_unlock_irqrestore(&pings.lock, flags);
}

static void icmp_echo_request_input(const struct ipv4_hdr *ip,
                                    const uint8_t *payload, uint16_t len) {
  uint8_t frame[ETH_FRAME_MAX];
  uint8_t *reply_payload = frame + IPV4_HEADROOM;
  memcpy(reply_payload, payload, len);

  /* Identifier, sequence, and body are echoed back unchanged - that is
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

void icmp_input(const struct ipv4_hdr *ip, const uint8_t *payload,
                uint16_t len) {
  if (!ip || !payload || len < ICMP_HDR_LEN || len > IPV4_PAYLOAD_MAX)
    return;

  const struct icmp_hdr *icmp = (const struct icmp_hdr *)payload;
  if (icmp->code != 0)
    return;

  /* The driver version this replaces skipped the inbound checksum and
   * echoed whatever arrived. Verifying it costs one pass over a packet we
   * are about to copy anyway, and without it a corrupted request comes
   * back as a well-formed reply that makes the link look healthy. */
  if (inet_checksum(payload, len) != 0)
    return;

  switch (icmp->type) {
  case ICMP_ECHO_REQUEST:
    icmp_echo_request_input(ip, payload, len);
    break;
  case ICMP_ECHO_REPLY:
    icmp_echo_reply_input(ip, icmp);
    break;
  default:
    break;
  }
}

/* ---------------- ping ------------------------------------------------- */

static void icmp_session_release(int slot) {
  uint64_t flags = spin_lock_irqsave(&pings.lock);
  pings.slot[slot].in_use = 0;
  spin_unlock_irqrestore(&pings.lock, flags);
}

long icmp_ping(const uint8_t dst[IPV4_ALEN], uint16_t ident, uint16_t seq,
               uint32_t timeout_ms, uint32_t *rtt_ms_out) {
  if (!dst || !netif_get())
    return -2;

  /* Claim a slot before transmitting. The reply can arrive while this
   * task is still inside ipv4_output_framed on another CPU, so the slot
   * has to be findable by then or the reply is dropped as unmatched. */
  int slot = -1;
  uint64_t flags = spin_lock_irqsave(&pings.lock);
  for (int i = 0; i < ICMP_PING_SESSIONS; i++) {
    if (pings.slot[i].in_use)
      continue;
    slot = i;
    pings.slot[i].in_use = 1;
    pings.slot[i].replied = 0;
    pings.slot[i].ident = ident;
    pings.slot[i].seq = seq;
    pings.slot[i].reply_ticks = 0;
    pings.slot[i].sent_ticks = pit_ticks();
    memcpy(pings.slot[i].dst, dst, IPV4_ALEN);
    break;
  }
  spin_unlock_irqrestore(&pings.lock, flags);

  if (slot < 0)
    return -2;

  uint16_t len = ICMP_HDR_LEN + ICMP_PING_PAYLOAD;
  uint8_t frame[ETH_FRAME_MAX];
  uint8_t *payload = frame + IPV4_HEADROOM;

  struct icmp_hdr *req = (struct icmp_hdr *)payload;
  req->type = ICMP_ECHO_REQUEST;
  req->code = 0;
  req->ident = to_be16(ident);
  req->sequence = to_be16(seq);

  /* The classic incrementing byte pattern. Any filler would do, but a
   * pattern makes a corrupted reply obvious in a hex dump instead of
   * looking like a short read. */
  for (uint16_t i = 0; i < ICMP_PING_PAYLOAD; i++)
    payload[ICMP_HDR_LEN + i] = (uint8_t)('a' + (i % 23));

  req->checksum = 0;
  req->checksum = inet_checksum(payload, len);

  /* 1 means parked behind an ARP request, which still goes out once the
   * reply lands -- only a negative return is a real failure. */
  if (ipv4_output_framed(frame, dst, IPPROTO_ICMP, len) < 0) {
    icmp_session_release(slot);
    return -2;
  }

  uint32_t hz = pit_get_freq();
  if (hz == 0)
    hz = 1000;
  uint64_t limit = ((uint64_t)timeout_ms * hz) / 1000u;
  if (limit == 0)
    limit = 1;

  uint64_t start = pit_ticks();
  for (;;) {
    int replied;
    uint64_t rtt;

    flags = spin_lock_irqsave(&pings.lock);
    replied = pings.slot[slot].replied;
    rtt = pings.slot[slot].reply_ticks - pings.slot[slot].sent_ticks;
    spin_unlock_irqrestore(&pings.lock, flags);

    if (replied) {
      if (rtt_ms_out)
        *rtt_ms_out = icmp_ticks_to_ms(rtt);
      icmp_session_release(slot);
      return 0;
    }

    if (pit_ticks() - start >= limit)
      break;

    /* Yield rather than spin: the reply is delivered by the driver poll
     * task, which cannot run until this one gives up the CPU. */
    task_yield();
  }

  icmp_session_release(slot);
  return -1;
}
