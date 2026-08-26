#include <net/ksocket.h>
#include <utilities/string.h>
#include <stdbool.h>

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);
extern void* memset(void* ptr, int value, size_t num);
extern void* memcpy(void* dest, const void* src, size_t num);

/* Active sockets use a linear lookup. */
static socket_t *socket_list = NULL;
static uint32_t next_socket_id = 1;

static bool ring_buffer_empty(ring_buffer_t *rb) {
    return rb->head == rb->tail;
}

static bool ring_buffer_full(ring_buffer_t *rb) {
    return ((rb->head + 1) % SOCKET_RX_BUFFER_SIZE) == rb->tail;
}

static int ring_buffer_push(ring_buffer_t *rb, const void *data, size_t len) {
    /* TODO: Reject writes larger than the available space. */
    if (ring_buffer_full(rb)) return -1;

    uint8_t *src = (uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        rb->data[rb->head] = src[i];
        rb->head = (rb->head + 1) % SOCKET_RX_BUFFER_SIZE;
    }
    return len;
}

static int ring_buffer_pop(ring_buffer_t *rb, void *data, size_t len) {
    if (ring_buffer_empty(rb)) return 0;

    uint8_t *dest = (uint8_t*)data;
    size_t i = 0;
    while (i < len && !ring_buffer_empty(rb)) {
        dest[i] = rb->data[rb->tail];
        rb->tail = (rb->tail + 1) % SOCKET_RX_BUFFER_SIZE;
        i++;
    }
    return i;
}

socket_t* socket_create(int type, int protocol) {
    socket_t *sock = (socket_t*)kmalloc(sizeof(socket_t));
    if (!sock) return NULL;

    memset(sock, 0, sizeof(socket_t));
    sock->id = next_socket_id++;
    sock->state = SOCKET_UNBOUND;
    sock->type = type;
    sock->protocol = protocol;

    /* TODO: Protect the socket list for SMP. */
    sock->next = socket_list;
    socket_list = sock;

    return sock;
}

int socket_bind(socket_t *sock, const sockaddr_in_t *addr) {
    if (!sock || !addr) return -1;

    sock->local = *addr;
    sock->state = SOCKET_OPEN;

    return 0;
}

int socket_sendto(socket_t *sock, const void *buf, size_t len, const sockaddr_in_t *dest_addr) {
    if (!sock || !buf || !dest_addr) return -1;

    sock->remote = *dest_addr;

    /* TODO: Send through the IP layer instead of reporting stub success. */
    return len;
}

int socket_recvfrom(socket_t *sock, void *buf, size_t len, sockaddr_in_t *src_addr) {
    if (!sock || !buf) return -1;

    /* TODO: Block until data arrives. */

    int bytes_read = ring_buffer_pop(&sock->rx_buffer, buf, len);

    if (src_addr && bytes_read > 0) {
        *src_addr = sock->remote;
    }

    return bytes_read;
}

/* Deliver an IP payload to its bound socket. */
void socket_handle_incoming(ipv4_addr_t src_ip, port_t src_port,
                            ipv4_addr_t dest_ip, port_t dest_port,
                            uint8_t protocol, void *payload, size_t length) {

    socket_t *curr = socket_list;
    socket_t *target = NULL;

    while (curr) {
        if (curr->protocol == protocol && curr->local.port == dest_port) {
            ipv4_addr_t wildcard = {{0,0,0,0}};

            if (memcmp(&curr->local.ip, &dest_ip, sizeof(ipv4_addr_t)) == 0 ||
                memcmp(&curr->local.ip, &wildcard, sizeof(ipv4_addr_t)) == 0) {

                target = curr;
                break;
            }
        }
        curr = curr->next;
    }

    if (!target) {
        return;
    }

    target->remote.ip = src_ip;
    target->remote.port = src_port;
    target->state = SOCKET_ESTABLISHED;

    /* TODO: Wake tasks waiting on this socket. */
    ring_buffer_push(&target->rx_buffer, payload, length);
}
