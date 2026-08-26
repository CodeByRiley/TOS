/* kernel/net/netif.c — see netif.h. */
#include <net/netif.h>
#include <utilities/string.h>

static struct netif interface;
static int registered;

void netif_register(const struct netif *nif) {
  if (!nif || !nif->tx)
    return;
  interface = *nif;
  if (interface.mtu == 0)
    interface.mtu = 1500;
  registered = 1;
}

struct netif *netif_get(void) { return registered ? &interface : (void *)0; }

void netif_set_ipv4(const uint8_t ipv4[IPV4_ALEN],
                    const uint8_t netmask[IPV4_ALEN],
                    const uint8_t gateway[IPV4_ALEN]) {
  if (!registered)
    return;
  if (ipv4)
    memcpy(interface.ipv4, ipv4, IPV4_ALEN);
  if (netmask)
    memcpy(interface.netmask, netmask, IPV4_ALEN);
  if (gateway)
    memcpy(interface.gateway, gateway, IPV4_ALEN);
}

int netif_tx(const void *frame, uint16_t len) {
  if (!registered || !frame || len == 0)
    return -1;
  return interface.tx(interface.driver_data, frame, len);
}
