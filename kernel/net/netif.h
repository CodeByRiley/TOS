/* kernel/net/netif.h , the bound network interface.
 *
 * One interface. TOS binds a single NIC, and giving this an index before
 * there is a second card to put in it would be structure without a user ,
 * the same call netmon.c made for its capture ring. When a second card
 * appears, netif_get() grows into netif_at(i) and the protocol files take
 * an interface argument; nothing else here changes shape.
 *
 * The point of the indirection is that ARP, IPv4, and ICMP now sit above
 * the driver instead of inside it. Before this, e1000.c answered ARP and
 * ICMP itself and e1000_send_frame was static, so nothing outside the
 * driver could put a frame on the wire at all: the replies worked only
 * because each one copied its destination MAC straight out of the request
 * it was answering. Nothing could speak first. That is the whole reason
 * this file exists.
 *
 * Implementation: kernel/net/netif.c.
 */
#ifndef NET_NETIF_H
#define NET_NETIF_H

#include <net/inet.h>
#include <stdint.h>
#include <utilities/types.h>

#define NETIF_MAC_LEN 6

struct netif {
  u8 mac[NETIF_MAC_LEN];
  u8 ipv4[IPV4_ALEN];
  u8 netmask[IPV4_ALEN];
  u8 gateway[IPV4_ALEN];

  /* Largest payload after the Ethernet header. */
  u16 mtu;

  /* Passed back to tx() untouched; the driver's own device record. */
  void *driver_data;

  /* Put one complete Ethernet frame on the wire. Returns 0 when the frame
   * was queued to hardware, -1 when it was not. Must not block: it is
   * called from the driver poll task and, once the e1000 IRQ path lands,
   * from interrupt context. */
  int (*tx)(void *driver_data, const void *frame, u16 len);
};

/* Take a copy of *nif as the bound interface. Safe to call again when the
 * address changes , DHCP will do exactly that. */
void netif_register(const struct netif *nif);

/* NULL until a driver has registered. Every protocol path checks this
 * rather than assuming a NIC exists, because the socket layer is reachable
 * from userspace on a machine with no network card at all. */
struct netif *netif_get(void);

/* Replace the addressing without disturbing the driver binding. */
void netif_set_ipv4(const u8 ipv4[IPV4_ALEN],
                    const u8 netmask[IPV4_ALEN],
                    const u8 gateway[IPV4_ALEN]);

/* Send one framed Ethernet frame. Returns 0 or -1. */
int netif_tx(const void *frame, u16 len);

#endif /* NET_NETIF_H */
