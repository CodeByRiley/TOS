#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdint.h>
#include "net/inet.h"
#include "net/ipv4.h"
#include "net/ksocket.h" // Included for struct ipv4_addr and struct packet_queue

#define UDP_HEADER_SIZE 8

struct udp_header {
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t length;
  uint16_t checksum;
} PACKED;

struct udp_packet {
  struct ipv4_hdr ip_header;
  struct udp_header udp_header;
  uint8_t data[];
} PACKED;


int udp_send(struct ipv4_addr *src, struct ipv4_addr *dst, uint16_t src_port, uint16_t dst_port, uint8_t *data, uint16_t len);
int udp_recv(struct packet_queue *queue, struct ipv4_addr *src, struct ipv4_addr *dst, uint16_t src_port, uint16_t dst_port, uint8_t *data, uint16_t len);

#endif
