/* kernel/net/arp.c — see arp.h. */
#include <devices/pit.h>
#include <net/arp.h>
#include <net/eth.h>
#include <net/netif.h>
#include <sync/spinlock.h>
#include <utilities/string.h>

#define ARP_CACHE_ENTRIES 16
#define ARP_PENDING_SLOTS 4
#define ARP_ENTRY_TTL_MS 120000u
#define ARP_RETRY_MS 1000u
#define ARP_MAX_RETRIES 3

enum arp_state { ARP_FREE = 0, ARP_PENDING, ARP_VALID };

struct arp_entry {
  uint8_t ip[IPV4_ALEN];
  uint8_t mac[ETH_ALEN];
  uint8_t state;
  uint8_t retries;
  /* Expiry while VALID, next retransmit while PENDING. One field because
   * an entry is never both, and two would invite updating the wrong one. */
  uint64_t deadline;
};

struct arp_pending {
  int in_use;
  int entry; /* index into cache[] */
  uint16_t type;
  uint16_t payload_len;
  uint64_t queued_at;
  uint8_t frame[ETH_FRAME_MAX];
};

static struct {
  spinlock_t lock;
  struct arp_entry cache[ARP_CACHE_ENTRIES];
  struct arp_pending pending[ARP_PENDING_SLOTS];
} arp = {.lock = SPINLOCK_INIT};

static uint64_t arp_deadline(uint32_t ms) {
  uint32_t hz = pit_get_freq();
  if (hz == 0)
    hz = 1000;
  uint64_t ticks = ((uint64_t)ms * hz) / 1000u;
  if (ticks == 0)
    ticks = 1;
  return pit_ticks() + ticks;
}

/* ---------------- cache, lock held ------------------------------------- */

static struct arp_entry *arp_find_locked(const uint8_t *ip) {
  for (int i = 0; i < ARP_CACHE_ENTRIES; i++) {
    if (arp.cache[i].state != ARP_FREE && ipv4_addr_equal(arp.cache[i].ip, ip))
      return &arp.cache[i];
  }
  return NULL;
}

static void arp_drop_pending_locked(int entry_index) {
  for (int i = 0; i < ARP_PENDING_SLOTS; i++) {
    if (arp.pending[i].in_use && arp.pending[i].entry == entry_index)
      arp.pending[i].in_use = 0;
  }
}

/* Free slot first, otherwise the entry closest to its deadline. Evicting by
 * deadline rather than round-robin keeps the gateway — refreshed on every
 * exchange — resident, which is the entry that actually matters. */
static struct arp_entry *arp_alloc_locked(void) {
  struct arp_entry *victim = &arp.cache[0];

  for (int i = 0; i < ARP_CACHE_ENTRIES; i++) {
    if (arp.cache[i].state == ARP_FREE)
      return &arp.cache[i];
    if (arp.cache[i].deadline < victim->deadline)
      victim = &arp.cache[i];
  }

  arp_drop_pending_locked((int)(victim - arp.cache));
  memset(victim, 0, sizeof(*victim));
  return victim;
}

/* Free slot first, otherwise the frame that has waited longest — it is the
 * one most likely to have been given up on upstream. */
static struct arp_pending *arp_pending_alloc_locked(void) {
  struct arp_pending *victim = &arp.pending[0];

  for (int i = 0; i < ARP_PENDING_SLOTS; i++) {
    if (!arp.pending[i].in_use)
      return &arp.pending[i];
    if (arp.pending[i].queued_at < victim->queued_at)
      victim = &arp.pending[i];
  }
  return victim;
}

/* ---------------- transmit --------------------------------------------- */

static void arp_fill(struct arp_ipv4 *pkt, const struct netif *nif,
                     uint16_t oper, const uint8_t *tha, const uint8_t *tpa) {
  memset(pkt, 0, sizeof(*pkt));
  pkt->htype = to_be16(ARP_HTYPE_ETHERNET);
  pkt->ptype = to_be16(ETH_TYPE_IPV4);
  pkt->hlen = ETH_ALEN;
  pkt->plen = IPV4_ALEN;
  pkt->oper = to_be16(oper);
  memcpy(pkt->sha, nif->mac, ETH_ALEN);
  memcpy(pkt->spa, nif->ipv4, IPV4_ALEN);
  if (tha)
    memcpy(pkt->tha, tha, ETH_ALEN);
  memcpy(pkt->tpa, tpa, IPV4_ALEN);
}

void arp_request(const uint8_t ip[IPV4_ALEN]) {
  struct netif *nif = netif_get();
  if (!nif || !ip)
    return;

  /* tha stays zero: it is exactly what the request is asking for. */
  struct arp_ipv4 req;
  arp_fill(&req, nif, ARP_OP_REQUEST, NULL, ip);
  eth_output(eth_broadcast, ETH_TYPE_ARP, &req, sizeof(req));
}

void arp_announce(void) {
  struct netif *nif = netif_get();
  if (!nif || ipv4_addr_is_zero(nif->ipv4))
    return;

  /* spa == tpa is what makes it gratuitous: the question and the answer
   * are the same address, so receivers treat it as an announcement. */
  struct arp_ipv4 pkt;
  arp_fill(&pkt, nif, ARP_OP_REQUEST, NULL, nif->ipv4);
  eth_output(eth_broadcast, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

void arp_prime_gateway(void) {
  struct netif *nif = netif_get();
  if (!nif || ipv4_addr_is_zero(nif->gateway))
    return;
  if (ipv4_addr_equal(nif->gateway, nif->ipv4))
    return;

  arp_request(nif->gateway);
}

/* Drain the frames parked behind `entry_index`. Called WITHOUT the lock:
 * transmitting under it would put a driver MMIO write inside a spinlock the
 * e1000 IRQ path will also take. One frame is copied out per pass so the
 * stack cost stays at a single 1514-byte buffer however deep the pool is. */
static void arp_flush_pending(int entry_index, const uint8_t mac[ETH_ALEN]) {
  for (;;) {
    uint8_t frame[ETH_FRAME_MAX];
    uint16_t payload_len = 0;
    uint16_t type = 0;
    int found = 0;

    uint64_t flags = spin_lock_irqsave(&arp.lock);
    for (int i = 0; i < ARP_PENDING_SLOTS; i++) {
      struct arp_pending *p = &arp.pending[i];
      if (!p->in_use || p->entry != entry_index)
        continue;
      memcpy(frame, p->frame, (size_t)ETH_HDR_LEN + p->payload_len);
      payload_len = p->payload_len;
      type = p->type;
      p->in_use = 0;
      found = 1;
      break;
    }
    spin_unlock_irqrestore(&arp.lock, flags);

    if (!found)
      return;
    eth_output_framed(frame, mac, type, payload_len);
  }
}

int arp_lookup(const uint8_t ip[IPV4_ALEN], uint8_t mac_out[ETH_ALEN]) {
  if (!ip || !mac_out)
    return -1;

  uint64_t flags = spin_lock_irqsave(&arp.lock);
  struct arp_entry *e = arp_find_locked(ip);
  int hit = e && e->state == ARP_VALID;
  if (hit)
    memcpy(mac_out, e->mac, ETH_ALEN);
  spin_unlock_irqrestore(&arp.lock, flags);

  return hit ? 0 : -1;
}

int arp_send_or_queue(const uint8_t ip[IPV4_ALEN], uint8_t *frame,
                      uint16_t payload_len, uint16_t type) {
  struct netif *nif = netif_get();
  if (!nif || !ip || !frame)
    return -1;
  if ((size_t)ETH_HDR_LEN + payload_len > ETH_FRAME_MAX)
    return -1;

  uint8_t mac[ETH_ALEN];
  int resolved = 0;
  int ask = 0;

  uint64_t flags = spin_lock_irqsave(&arp.lock);
  struct arp_entry *e = arp_find_locked(ip);

  if (e && e->state == ARP_VALID) {
    memcpy(mac, e->mac, ETH_ALEN);
    resolved = 1;
  } else {
    if (!e) {
      e = arp_alloc_locked();
      memcpy(e->ip, ip, IPV4_ALEN);
      e->state = ARP_PENDING;
      e->retries = 0;
      e->deadline = arp_deadline(ARP_RETRY_MS);
      /* Only the first miss asks; arp_tick owns the retransmits. */
      ask = 1;
    }

    struct arp_pending *p = arp_pending_alloc_locked();
    p->in_use = 1;
    p->entry = (int)(e - arp.cache);
    p->type = type;
    p->payload_len = payload_len;
    p->queued_at = pit_ticks();
    memcpy(p->frame, frame, (size_t)ETH_HDR_LEN + payload_len);
  }
  spin_unlock_irqrestore(&arp.lock, flags);

  if (resolved)
    return eth_output_framed(frame, mac, type, payload_len);

  if (ask)
    arp_request(ip);
  return 1;
}

/* ---------------- receive ---------------------------------------------- */

/* RFC 826: refresh an existing entry for the sender whatever the opcode,
 * and create one only when the packet was addressed to us. The refresh is
 * what saves a round trip on the common sequence where a host ARPs us and
 * then immediately sends the traffic it was resolving for. */
static void arp_learn(const uint8_t *ip, const uint8_t *mac, int may_create) {
  int entry_index = -1;

  uint64_t flags = spin_lock_irqsave(&arp.lock);
  struct arp_entry *e = arp_find_locked(ip);
  if (!e && may_create)
    e = arp_alloc_locked();
  if (e) {
    memcpy(e->ip, ip, IPV4_ALEN);
    memcpy(e->mac, mac, ETH_ALEN);
    e->state = ARP_VALID;
    e->retries = 0;
    e->deadline = arp_deadline(ARP_ENTRY_TTL_MS);
    entry_index = (int)(e - arp.cache);
  }
  spin_unlock_irqrestore(&arp.lock, flags);

  if (entry_index >= 0)
    arp_flush_pending(entry_index, mac);
}

void arp_input(const struct eth_hdr *eth, const uint8_t *payload,
               uint16_t len) {
  struct netif *nif = netif_get();
  if (!nif || !eth || !payload || len < sizeof(struct arp_ipv4))
    return;

  const struct arp_ipv4 *pkt = (const struct arp_ipv4 *)payload;
  if (from_be16(pkt->htype) != ARP_HTYPE_ETHERNET ||
      from_be16(pkt->ptype) != ETH_TYPE_IPV4 || pkt->hlen != ETH_ALEN ||
      pkt->plen != IPV4_ALEN)
    return;

  uint16_t oper = from_be16(pkt->oper);
  if (oper != ARP_OP_REQUEST && oper != ARP_OP_REPLY)
    return;

  int for_us = ipv4_addr_equal(pkt->tpa, nif->ipv4);

  /* A zero sender address means an ARP probe: the sender has no address
   * yet, so caching it would map 0.0.0.0 onto a real MAC. */
  if (!ipv4_addr_is_zero(pkt->spa))
    arp_learn(pkt->spa, pkt->sha, for_us);

  if (oper != ARP_OP_REQUEST || !for_us)
    return;
  if (ipv4_addr_is_zero(nif->ipv4))
    return;

  struct arp_ipv4 reply;
  arp_fill(&reply, nif, ARP_OP_REPLY, pkt->sha, pkt->spa);
  eth_output(eth->src, ETH_TYPE_ARP, &reply, sizeof(reply));
}

/* ---------------- maintenance ------------------------------------------ */

void arp_tick(void) {
  if (!netif_get())
    return;

  uint8_t retry_ips[ARP_CACHE_ENTRIES][IPV4_ALEN];
  int retry_count = 0;
  uint64_t now = pit_ticks();

  uint64_t flags = spin_lock_irqsave(&arp.lock);
  for (int i = 0; i < ARP_CACHE_ENTRIES; i++) {
    struct arp_entry *e = &arp.cache[i];
    if (e->state == ARP_FREE || now < e->deadline)
      continue;

    if (e->state == ARP_VALID) {
      e->state = ARP_FREE;
      continue;
    }

    if (e->retries >= ARP_MAX_RETRIES) {
      /* Nobody answered. Drop the parked frames rather than hold them for
       * a host that is not there; every caller has long since returned. */
      arp_drop_pending_locked(i);
      memset(e, 0, sizeof(*e));
      continue;
    }

    e->retries++;
    e->deadline = arp_deadline(ARP_RETRY_MS);
    memcpy(retry_ips[retry_count++], e->ip, IPV4_ALEN);
  }
  spin_unlock_irqrestore(&arp.lock, flags);

  for (int i = 0; i < retry_count; i++)
    arp_request(retry_ips[i]);
}

void arp_flush(void) {
  uint64_t flags = spin_lock_irqsave(&arp.lock);
  memset(arp.cache, 0, sizeof(arp.cache));
  for (int i = 0; i < ARP_PENDING_SLOTS; i++)
    arp.pending[i].in_use = 0;
  spin_unlock_irqrestore(&arp.lock, flags);
}
