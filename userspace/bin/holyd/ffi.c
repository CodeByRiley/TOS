#include "ffi.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Forward declarations
static HDValue native_udp_socket(int arg_count, HDValue* args);
static HDValue native_udp_bind(int arg_count, HDValue* args);
static HDValue native_udp_send(int arg_count, HDValue* args);
static HDValue native_udp_recv(int arg_count, HDValue* args);

// Private array mapping string names to C functions
static struct NativeDef {
    const char* name;
    NativeFn fn;
} native_functions[] = {
    { "UdpReceive", native_udp_recv   },
    { "UdpSocket",  native_udp_socket },
    { "UdpBind",    native_udp_bind   },
    { "UdpSend",    native_udp_send   },
    { NULL, NULL }
};

// Public lookup function
NativeFn ffi_lookup_native(const char* name, int len) {
    for (int i = 0; native_functions[i].name != NULL; i++) {
        int nlen = (int)strlen(native_functions[i].name);
        if (nlen == len && strncmp(name, native_functions[i].name, len) == 0) {
            return native_functions[i].fn;
        }
    }
    return NULL; // Not found
}

// --- Implementations ---

static HDValue native_udp_socket(int arg_count, HDValue* args) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return int_value((long long)fd);
}

static HDValue native_udp_bind(int arg_count, HDValue* args) {
    if (arg_count != 2) return int_value(-1);
    int fd = (int)args[0].i64;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)args[1].i64);
    addr.sin_addr.s_addr = INADDR_ANY;

    return int_value(bind(fd, (struct sockaddr*)&addr, sizeof(addr)));
}

static HDValue native_udp_send(int arg_count, HDValue* args) {
    if (arg_count != 4) return int_value(-1);
    int fd = (int)args[0].i64;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)args[3].i64);

    if (args[2].type == VAL_ARRAY && args[2].array_len == 4) {
        unsigned char ip[4];
        for (int i = 0; i < 4; i++) {
            ip[i] = (unsigned char)args[2].elements[i].i64;
        }
        dest.sin_addr.s_addr = *(uint32_t*)ip;
    }

    return int_value(sendto(fd, args[1].str, args[1].str_len, 0,
                            (struct sockaddr*)&dest, sizeof(dest)));
}

static HDValue native_udp_recv(int arg_count, HDValue* args) {
    if (arg_count != 1) return int_value(-1);
    int fd = (int)args[0].i64;
    char buf[1514];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    int bytes = recvfrom(fd, buf, sizeof(buf), 0,
                         (struct sockaddr*)&src, &src_len);
    if (bytes <= 0) {
        return string_value("", 0);
    }

    char* heap_buf = malloc(bytes);
    memcpy(heap_buf, buf, bytes);
    return string_value(heap_buf, bytes);
}
