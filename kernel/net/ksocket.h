#ifndef KSOCKET_H
#define KSOCKET_H

#include <stdint.h>
#include <stddef.h>

// Ensure tight packing so network headers align correctly
#pragma pack(push, 1)

// Standard IPv4 address
typedef struct {
    uint8_t bytes[4];
} ipv4_addr_t;

// Port number
typedef uint16_t port_t;

// Standard socket address (similar to sockaddr_in)
typedef struct {
    uint16_t family;       // AF_INET, etc.
    port_t port;           // Port number (network byte order)
    ipv4_addr_t ip;        // IPv4 address
    uint8_t padding[8];    // Padding to match standard struct sizes
} sockaddr_in_t;

// Socket Types
#define SOCK_STREAM 1 // TCP
#define SOCK_DGRAM  2 // UDP
#define SOCK_RAW    3 // Raw IP

// Protocols
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

// Socket States
typedef enum {
    SOCKET_UNBOUND,
    SOCKET_OPEN,
    SOCKET_LISTENING,
    SOCKET_CONNECTING,
    SOCKET_ESTABLISHED,
    SOCKET_CLOSED
} socket_state_t;

// Simple ring buffer for receiving data
#define SOCKET_RX_BUFFER_SIZE 4096
typedef struct {
    uint8_t data[SOCKET_RX_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
} ring_buffer_t;

// The core Socket Structure
typedef struct socket {
    uint32_t id;
    socket_state_t state;
    int type;               // SOCK_STREAM, SOCK_DGRAM
    int protocol;           // IPPROTO_UDP, IPPROTO_TCP

    sockaddr_in_t local;    // Local IP and Port
    sockaddr_in_t remote;   // Remote IP and Port (for connected sockets)

    ring_buffer_t rx_buffer;
    // spinlock_t lock;      // TODO: Add a spinlock for SMP safety

    struct socket *next;    // Linked list for the global socket table
} socket_t;

#pragma pack(pop)

// API Functions
socket_t* socket_create(int type, int protocol);
int socket_bind(socket_t *sock, const sockaddr_in_t *addr);
int socket_recvfrom(socket_t *sock, void *buf, size_t len, sockaddr_in_t *src_addr);
int socket_sendto(socket_t *sock, const void *buf, size_t len, const sockaddr_in_t *dest_addr);

// Called by the IP layer when a packet arrives
void socket_handle_incoming(ipv4_addr_t src_ip, port_t src_port,
                            ipv4_addr_t dest_ip, port_t dest_port,
                            uint8_t protocol, void *payload, size_t length);

#endif /* KSOCKET_H */
