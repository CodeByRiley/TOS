#include "ffi.h"
#include "bin/holyd/compiler.h"
#include "wm.h"
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define ival(x) int_value(x)

// Forward declarations
static HDValue native_udp_socket(int arg_count, HDValue *args);
static HDValue native_udp_bind(int arg_count, HDValue *args);
static HDValue native_udp_send(int arg_count, HDValue *args);
static HDValue native_udp_recv(int arg_count, HDValue *args);
static HDValue native_win_create(int arg_count, HDValue *args);
static HDValue native_win_set_title(int arg_count, HDValue *args);
static HDValue native_win_invalidate(int arg_count, HDValue *args);
static HDValue native_win_destroy(int arg_count, HDValue *args);
static HDValue native_win_draw_text(int arg_count, HDValue *args);
static HDValue native_win_draw_rect(int arg_count, HDValue *args);
static HDValue native_win_present(int arg_count, HDValue *args);

// Private array mapping string names to C functions
static struct NativeDef {
  const char *name;
  NativeFn fn;
} native_functions[] = {{"UdpReceive", native_udp_recv},
                        {"UdpSocket", native_udp_socket},
                        {"UdpBind", native_udp_bind},
                        {"UdpSend", native_udp_send},
                        { "WinCreate",  native_win_create },
                        { "WinSetTitle", native_win_set_title },
                        { "WinInvalidate", native_win_invalidate },
                        { "WinDestroy", native_win_destroy },
                        {NULL, NULL}};

// Public lookup function
NativeFn ffi_lookup_native(const char *name, int len) {
  for (int i = 0; native_functions[i].name != NULL; i++) {
    int nlen = (int)strlen(native_functions[i].name);
    if (nlen == len && strncmp(name, native_functions[i].name, len) == 0) {
      return native_functions[i].fn;
    }
  }
  return NULL; // Not found
}

// --- Implementations ---

static HDValue native_udp_socket(int arg_count, HDValue *args) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  return int_value((long long)fd);
}

static HDValue native_udp_bind(int arg_count, HDValue *args) {
  if (arg_count != 2)
    return int_value(-1);
  int fd = (int)args[0].i64;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)args[1].i64);
  addr.sin_addr.s_addr = INADDR_ANY;

  return int_value(bind(fd, (struct sockaddr *)&addr, sizeof(addr)));
}

static HDValue native_udp_send(int arg_count, HDValue *args) {
  if (arg_count != 4)
    return int_value(-1);
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
    dest.sin_addr.s_addr = *(uint32_t *)ip;
  }

  return int_value(sendto(fd, args[1].str, args[1].str_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest)));
}

static HDValue native_udp_recv(int arg_count, HDValue *args) {
  if (arg_count != 1) {
    return int_value(-1);
  }

  int fd = (int)args[0].i64;
  char buf[1514];

  struct sockaddr_in src;
  memset(&src, 0, sizeof(src));

  socklen_t src_len = sizeof(src);

  int bytes;

  do {
    bytes =
        recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
  } while (bytes < 0 && errno == EINTR);

  if (bytes < 0) {
    printf("[UDP] recvfrom failed: fd=%d errno=%d (%s)\n", fd, errno,
           strerror(errno));

    return string_value("", 0);
  }

  char *heap_buf = malloc((size_t)bytes + 1);

  if (heap_buf == NULL) {
    return string_value("", 0);
  }

  memcpy(heap_buf, buf, (size_t)bytes);
  heap_buf[bytes] = '\0';

  printf("[UDP] received %d bytes\n", bytes);

  return string_value(heap_buf, bytes);
}

// Windowing

// TODO: This is a mess.


// WinCreate(I64 w, I64 h, string title) -> I64 handle
static HDValue native_win_create(int arg_count, HDValue* args) {
    if (arg_count != 3) return int_value(-1);

    int w = (int)args[0].i64;
    int h = (int)args[1].i64;

    // HolyD strings aren't null-terminated, so copy it
    int tlen = args[2].str_len;
    char* title = (char*)malloc(tlen + 1);
    memcpy(title, args[2].str, tlen);
    title[tlen] = '\0';

    struct wm_window win;
    // Call the userspace C API
    int result = wm_window_create(w, h, title, &win);
    free(title);

    if (result != 0) {
        return int_value(-1); // Failed
    }

    // Return the integer handle to HolyD!
    return int_value((long long)win.handle);
}

// WinSetTitle(I64 handle, string title) -> I64 result
static HDValue native_win_set_title(int arg_count, HDValue* args) {
    if (arg_count != 2) return int_value(-1);

    int handle = (int)args[0].i64;
    int tlen = args[1].str_len;
    char* title = (char*)malloc(tlen + 1);
    memcpy(title, args[1].str, tlen);
    title[tlen] = '\0';

    int result = wm_window_set_title(handle, title);
    free(title);
    return int_value(result);
}

// WinInvalidate(I64 handle) -> I64 result
static HDValue native_win_invalidate(int arg_count, HDValue* args) {
    if (arg_count != 1) return int_value(-1);
    int handle = (int)args[0].i64;
    return int_value(wm_window_invalidate(handle));
}

// WinDestroy(I64 handle) -> I64 result
static HDValue native_win_destroy(int arg_count, HDValue* args) {
    if (arg_count != 1) return int_value(-1);
    int handle = (int)args[0].i64;
    return int_value(wm_window_destroy(handle));
}

static HDValue native_win_draw_text(int arg_count, HDValue *args) {
  return int_value(-1); // TODO: Finish this.
}
static HDValue native_win_draw_rect(int arg_count, HDValue *args) {
  return int_value(-1); // TODO: Finish this.
}
static HDValue native_win_present(int arg_count, HDValue *args) {
  return int_value(-1); // TODO: Finish this.
}
