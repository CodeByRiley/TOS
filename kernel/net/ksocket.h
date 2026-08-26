#ifndef KSOCKET_H
#define KSOCKET_H

#include "sync/spinlock.h"
#include <stdint.h>
#include <stddef.h>

#define AF_INET 2


// Port number
typedef uint16_t port_t;

// NETWORK WIRE FORMATS
#pragma pack(push, 1)

// Standard IPv4 address
struct ipv4_addr {
    uint8_t bytes[4];
};

// Standard socket address
struct sockaddr_in {
    uint16_t family;       // AF_INET, etc.
    port_t port;           // Port number (network byte order)
    struct ipv4_addr ip;   // IPv4 address
    uint8_t padding[8];    // Padding to match standard struct sizes
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
    uint16_t length;
    uint8_t *payload;          // Pointer to the packet data in memory
    struct packet_node *next;
};

// A FIFO queue for holding received packets
struct packet_queue {
    struct packet_node *head;
    struct packet_node *tail;
    uint32_t count;
};

// --------------------------------

#define SOCKET_RX_BUFFER_SIZE 4096
struct ring_buffer {
    uint8_t data[SOCKET_RX_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
};

// The core Socket Structure
struct socket {
    uint32_t id;
    enum socket_state state;
    int type;               // SOCK_STREAM, SOCK_DGRAM
    int protocol;           // IPPROTO_UDP, IPPROTO_TCP

    struct sockaddr_in local;    // Local IP and Port
    struct sockaddr_in remote;  // Remote IP and Port

    // RX structures
    struct ring_buffer rx_buffer;   // For TCP byte streams
    struct packet_queue rx_queue;   // For UDP datagrams

    struct spinlock lock;
    struct socket *next;    // Linked list for the global socket table
};

// API Function
void udp_echo_thread(void);
struct socket* socket_create(int type, int protocol);
int socket_bind(struct socket *sock, const struct sockaddr_in *addr);
int socket_recvfrom(struct socket *sock, void *buf, size_t len, struct sockaddr_in *src_addr);
int socket_sendto(struct socket *sock, const void *buf, size_t len, const struct sockaddr_in *dest_addr);

// Called by the IP layer when a packet arrives
void socket_handle_incoming(struct ipv4_addr src_ip, port_t src_port,
                            struct ipv4_addr dest_ip, port_t dest_port,
                            uint8_t protocol, void *payload, size_t length);

#endif /* KSOCKET_H */
