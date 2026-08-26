#include "sched/sched.h"
#include "sync/spinlock.h"
#include "utilities/log.h"
#include "utilities/types.h"
#include <net/ksocket.h>
#include <net/ipv4.h>
#include <net/udp.h>
#include <stdbool.h>
#include <utilities/string.h>

extern void *kmalloc(size_t size);
extern void kfree(void *ptr);
extern void *memset(void *ptr, int value, size_t num);
extern void *memcpy(void *dest, const void *src, size_t num);

/* Active sockets use a linear lookup. */
static struct socket *socket_list = NULL;
static struct spinlock socket_list_lock = SPINLOCK_INIT;
static uint32_t next_socket_id = 1;


void udp_echo_thread(void) {
    // Create and bind a socket to port 5000
    struct socket *sock = socket_create(SOCK_DGRAM, IPPROTO_UDP);
    if (!sock) return;

    struct sockaddr_in bind_addr = {0};
    bind_addr.family = AF_INET; // Make sure AF_INET is defined (usually 2)
    bind_addr.port = to_be16(5000);
    bind_addr.ip.bytes[0] = 0; // Bind to 0.0.0.0 (any)
    bind_addr.ip.bytes[1] = 0;
    bind_addr.ip.bytes[2] = 0;
    bind_addr.ip.bytes[3] = 0;

    if (socket_bind(sock, &bind_addr) != 0) {
        log_write("Echo thread: failed to bind", KERNEL, LOG_ERROR);
        return;
    }

    log_write("Echo thread: listening on port 5000", KERNEL, LOG_INFO);

    char buf[512];
    struct sockaddr_in src_addr;

    while (1) {
        // Block (or poll) waiting for data
        int bytes = socket_recvfrom(sock, buf, 512, &src_addr);

        if (bytes > 0) {
            buf[bytes] = '\0'; // Null terminate for printing
            log_write_fmt(KERNEL, LOG_INFO, "Echo thread: received '%s', bouncing back!", buf);

            // Return to sender ;p
            socket_sendto(sock, buf, bytes, &src_addr);
        }

        // Yield so we don't lock up the CPU if recvfrom returns immediately
        task_yield();
    }
}

struct socket *socket_create(int type, int protocol) {
  struct socket *sock = (struct socket *)kmalloc(sizeof(struct socket));
  if (!sock)
    return NULL;

  memset(sock, 0, sizeof(struct socket));

  // Safely assign ID and add to list
  uint64_t rflags = spin_lock_irqsave(&socket_list_lock);
  sock->id = next_socket_id++;
  sock->next = socket_list;
  socket_list = sock;
  spin_unlock_irqrestore(&socket_list_lock, rflags);

  sock->state = SOCKET_UNBOUND;
  sock->type = type;
  sock->protocol = protocol;

  spinlock_init(&sock->lock);

  return sock;
}

int socket_bind(struct socket *sock, const struct sockaddr_in *addr) {
  if (!sock || !addr)
    return -1;

  sock->local = *addr;
  sock->state = SOCKET_OPEN;

  return 0;
}

int socket_sendto(struct socket *sock, const void *buf, size_t len,
                  const struct sockaddr_in *dest_addr) {
    if (!sock || !buf || !dest_addr) return -1;
    if (len > (IPV4_PAYLOAD_MAX - UDP_HEADER_SIZE)) return -1;

    sock->remote = *dest_addr;

    size_t total_frame_size = IPV4_HEADROOM + UDP_HEADER_SIZE + len;
    uint8_t *frame = kmalloc(total_frame_size);
    if (!frame) return -1;

    uint8_t *payload_dest = frame + IPV4_HEADROOM + UDP_HEADER_SIZE;
    memcpy(payload_dest, buf, len);

    struct udp_header *udp = (struct udp_header *)(frame + IPV4_HEADROOM);
    udp->src_port = sock->local.port;
    udp->dst_port = dest_addr->port;
    udp->length = to_be16(UDP_HEADER_SIZE + len);
    udp->checksum = 0;

    int ret = ipv4_output(dest_addr->ip.bytes, IPPROTO_UDP,
                          frame + IPV4_HEADROOM, UDP_HEADER_SIZE + len);

    kfree(frame);

    if (ret < 0) return -1;
    return len;
}

int socket_recvfrom(struct socket *sock, void *buf, size_t len, struct sockaddr_in *src_addr) {
    if (!sock || !buf) return -1;

    // TODO: Block until data arrives.

    // LOCK THE SOCKET, NOT THE GLOBAL LIST!
    uint64_t rflags = spin_lock_irqsave(&sock->lock);

    if (!sock->rx_queue.head) {
        spin_unlock_irqrestore(&sock->lock, rflags);
        return 0; // No data
    }

    // Pop the head node
    struct packet_node *node = sock->rx_queue.head;
    sock->rx_queue.head = node->next;
    if (!sock->rx_queue.head) {
        sock->rx_queue.tail = NULL;
    }
    sock->rx_queue.count--;

    // UNLOCK BEFORE COPYING TO USER SPACE
    spin_unlock_irqrestore(&sock->lock, rflags);

    size_t to_copy = (len < node->length) ? len : node->length;
    memcpy(buf, node->payload, to_copy);

    if (src_addr) {
        src_addr->family = AF_INET;
        src_addr->port = node->src_port;
        src_addr->ip = node->src_ip;
    }

    kfree(node->payload);
    kfree(node);

    return to_copy;
}

/* Unlink, drain, free. The queued packets each own a kmalloc'd payload, so
 * dropping the socket without walking the queue leaks both. */
void socket_close(struct socket *sock) {
  if (!sock)
    return;

  uint64_t rflags = spin_lock_irqsave(&socket_list_lock);
  struct socket **link = &socket_list;
  while (*link && *link != sock)
    link = &(*link)->next;
  if (*link == sock)
    *link = sock->next;
  spin_unlock_irqrestore(&socket_list_lock, rflags);

  /* Off the list now, so socket_handle_incoming can no longer find it and
   * the queue cannot grow underneath this walk. */
  struct packet_node *node = sock->rx_queue.head;
  while (node) {
    struct packet_node *next = node->next;
    kfree(node->payload);
    kfree(node);
    node = next;
  }

  kfree(sock);
}

/* Linux's ephemeral range. Linear scan of the socket list: the table is a
 * list of a handful of entries, and a bitmap would be structure without a
 * user until something opens sockets in bulk. */
port_t socket_alloc_ephemeral_port(void) {
  static uint16_t next_port = 32768;

  for (int attempt = 0; attempt < 28232; attempt++) {
    uint16_t candidate = next_port;
    next_port = (next_port >= 60999u) ? 32768u : (uint16_t)(next_port + 1u);

    int taken = 0;
    uint64_t rflags = spin_lock_irqsave(&socket_list_lock);
    for (struct socket *s = socket_list; s; s = s->next) {
      if (from_be16(s->local.port) == candidate) {
        taken = 1;
        break;
      }
    }
    spin_unlock_irqrestore(&socket_list_lock, rflags);

    if (!taken)
      return to_be16(candidate);
  }
  return 0;
}

/* Deliver an IP payload to its bound socket. */
void socket_handle_incoming(struct ipv4_addr src_ip, port_t src_port,
                            struct ipv4_addr dest_ip, port_t dest_port,
                            uint8_t protocol, void *payload, size_t length) {

  struct socket *curr = socket_list;
  struct socket *target = NULL;

  // 1. Lock the global list just to find the target socket
  uint64_t list_rflags = spin_lock_irqsave(&socket_list_lock);

  while (curr) {
    if (curr->protocol == protocol && curr->local.port == dest_port) {
      struct ipv4_addr wildcard = {{0, 0, 0, 0}};
      if (memcmp(&curr->local.ip, &dest_ip, sizeof(struct ipv4_addr)) == 0 ||
          memcmp(&curr->local.ip, &wildcard, sizeof(struct ipv4_addr)) == 0) {
        target = curr;
        break;
      }
    }
    curr = curr->next;
  }

  // Unlock the global list immediately!
  spin_unlock_irqrestore(&socket_list_lock, list_rflags);

  if (!target)
    return;

  // 2. Allocate memory BEFORE taking the target lock
  struct packet_node *node = kmalloc(sizeof(struct packet_node));
  if (!node) return; // Out of memory

  node->payload = kmalloc(length);
  if (!node->payload) {
    kfree(node);
    return;
  }

  memcpy(node->payload, payload, length);
  node->src_ip = src_ip;
  node->src_port = src_port;
  node->length = length;
  node->next = NULL;

  // 3. Lock the specific socket to safely push to its queue
  uint64_t sock_rflags = spin_lock_irqsave(&target->lock);

  if (target->rx_queue.tail) {
    target->rx_queue.tail->next = node;
  } else {
    target->rx_queue.head = node;
  }
  target->rx_queue.tail = node;
  target->rx_queue.count++;

  spin_unlock_irqrestore(&target->lock, sock_rflags);

  // Update socket metadata
  target->remote.ip = src_ip;
  target->remote.port = src_port;
  target->state = SOCKET_ESTABLISHED;

  /* TODO: Wake tasks waiting on this socket. */
}
