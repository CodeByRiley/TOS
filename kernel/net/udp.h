#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdint.h>
#include "net/inet.h"
#include "net/ipv4.h"
#include "net/ksocket.h" // Included for struct ipv4_addr and struct packet_queue
#include <utilities/types.h>

#define UDP_HEADER_SIZE 8

struct udp_header {
  u16 src_port;
  u16 dst_port;
  u16 length;
  u16 checksum;
} PACKED;

struct udp_packet {
  struct ipv4_hdr ip_header;
  struct udp_header udp_header;
  u8 data[];
} PACKED;

#endif
