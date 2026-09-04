#ifndef KSOCKET_H
#define KSOCKET_H

#include "sync/spinlock.h"
#include <stdint.h>
#include <stddef.h>
#include <utilities/types.h>

#define AF_INET 2


// Port number
typedef u16 port_t;

// NETWORK WIRE FORMATS
#pragma pack(push, 1)

// Standard IPv4 address
struct ipv4_addr {
    u8 bytes[4];
};

// Standard socket address
struct sockaddr_in {
    u16 family;       // AF_INET, etc.
    port_t port;           // Port number (network byte order)
    struct ipv4_addr ip;   // IPv4 address
    u8 padding[8];    // Padding to match standard struct sizes
};

#pragma pack(pop)



// Socket Types
#define SOCK_STREAM 1 // TCP
#define SOCK_DGRAM  2 // UDP
#define SOCK_RAW    3 // Raw IP

// Socket States
enum socket_state {
    SOCKET_UNBOUND,
    SOCKET_OPEN,
    SOCKET_LISTENING,
    SOCKET_CONNECTING,
    SOCKET_ESTABLISHED,
    SOCKET_CLOSED
};

// --- Generic Packet Queue ---

// A node representing a single received packet in a queue
struct packet_node {
    struct ipv4_addr src_ip;
    port_t src_port;
    u16 length;
    u8 *payload;          // Pointer to the packet data in memory
    struct packet_node *next;
};

// A FIFO queue for holding received packets
struct packet_queue {
    struct packet_node *head;
    struct packet_node *tail;
    u32 count;
};

// --------------------------------

// The core Socket Structure
struct socket {
    u32 id;
    enum socket_state state;
    int type;               // SOCK_STREAM, SOCK_DGRAM
    int protocol;           // IPPROTO_UDP, IPPROTO_TCP

    struct sockaddr_in local;    // Local IP and Port
    struct sockaddr_in remote;  // Remote IP and Port

    struct packet_queue rx_queue;

    struct spinlock lock;
    struct socket *next;    // Linked list for the global socket table
};

// API Function
void udp_echo_thread(void);
struct socket* socket_create(int type, int protocol);
int socket_bind(struct socket *sock, const struct sockaddr_in *addr);
int socket_recvfrom(struct socket *sock, void *buf, usize len, struct sockaddr_in *src_addr);
int socket_sendto(struct socket *sock, const void *buf, usize len, const struct sockaddr_in *dest_addr);

/* Unlink from the global table, drain any queued datagrams, and free.
 * Without this every socket and every packet still queued on it leaked for
 * the life of the kernel. */
void socket_close(struct socket *sock);

/* Pick an unused ephemeral port. Returns 0 when none is free. */
port_t socket_alloc_ephemeral_port(void);

// Called by the IP layer when a packet arrives
void socket_handle_incoming(struct ipv4_addr src_ip, port_t src_port,
                            struct ipv4_addr dest_ip, port_t dest_port,
                            u8 protocol, void *payload, usize length);

#endif /* KSOCKET_H */
