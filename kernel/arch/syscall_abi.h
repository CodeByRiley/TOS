/* kernel/arch/syscall_abi.h , data copied across the syscall boundary.
 *
 * This is the one definition of each native TOS ABI payload. Kernel headers
 * and libtos both include it; changing field order or width therefore changes
 * both consumers in the same compilation rather than relying on mirrored
 * structs and size-only assertions.
 */
#ifndef SYSCALL_ABI_H
#define SYSCALL_ABI_H

#include <stddef.h>
#include <stdint.h>

#define STAT_TYPE_FILE 0
#define STAT_TYPE_DIR  1

struct stat_user {
  uint64_t size;
  uint64_t first_cluster;
  uint32_t type;
  uint32_t attr;
};

#define PROC_NAME_MAX 16

struct proc_info {
  uint64_t ticks_run;
  int32_t pid;
  int32_t parent_pid;
  int32_t state;
  char name[PROC_NAME_MAX];
};

#define PROC_STATE_RUNNING  0
#define PROC_STATE_BLOCKED  1
#define PROC_STATE_ZOMBIE   2
#define PROC_STATE_READY    3
#define PROC_STATE_DEAD     4
#define PROC_STATE_SLEEPING 5
#define PROC_STATE_LOADING  6

struct mem_stats {
  uint64_t total_frames;
  uint64_t used_frames;
  uint64_t frame_size;
};

#define NET_FRAME_BYTES 128
#define NET_CAPTURE_BATCH 16
#define NET_DIR_RX 0
#define NET_DIR_TX 1

struct net_frame {
  uint64_t seq;
  uint64_t ticks;
  uint32_t length;
  uint32_t captured;
  uint32_t direction;
  uint32_t reserved;
  uint8_t data[NET_FRAME_BYTES];
};

struct net_stats {
  uint64_t rx_frames;
  uint64_t rx_bytes;
  uint64_t tx_frames;
  uint64_t tx_bytes;
  uint64_t seq_next;
  uint64_t seq_oldest;
  uint8_t mac[6];
  uint8_t ipv4[4];
  uint32_t link_up;
  uint32_t speed_mbps;
  uint32_t present;
  uint32_t ring_frames;
};

struct net_ping {
  uint8_t dst[4];
  uint16_t ident;
  uint16_t seq;
  uint32_t timeout_ms;
  uint32_t rtt_ms;
};

#define AUDIO_FORMAT_S16_LE 1

struct audio_status {
  uint32_t available;
  uint32_t playing;
  uint32_t paused;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t format;
  uint32_t ring_capacity;
  uint32_t ring_queued;
  uint32_t device_queued;
  uint32_t underruns;
  uint32_t volume;
  int32_t owner_pid;
};

#define MSG_NONE       0
#define MSG_KEY_DOWN   1
#define MSG_KEY_UP     2
#define MSG_MOUSE_MOVE 3
#define MSG_MOUSE_DOWN 4
#define MSG_MOUSE_UP   5
#define MSG_TIMER      6
#define MSG_QUIT       7

struct msg {
  uint16_t type;
  uint16_t param;
  int16_t x;
  int16_t y;
  uint32_t when;
};

#define IPC_PEER_EXITED 0x180
#define IPC_USER_FIRST  0x200

struct ipc_msg {
  uint32_t type;
  uint32_t from_pid;
  int32_t a, b, c, d;
  uint64_t va;
  uint32_t pitch;
  uint32_t flags;
  char str[48];
};

struct fb_info {
  uint64_t width, height, pitch, bpp;
};

#define FB_PRESENT_MAX_RECTS 16

struct fb_rect {
  uint32_t x, y, w, h;
};

_Static_assert(sizeof(struct stat_user) == 24, "stat_user ABI");
_Static_assert(offsetof(struct stat_user, type) == 16, "stat_user.type ABI");
_Static_assert(sizeof(struct proc_info) == 40, "proc_info ABI");
_Static_assert(offsetof(struct proc_info, pid) == 8, "proc_info.pid ABI");
_Static_assert(offsetof(struct proc_info, name) == 20, "proc_info.name ABI");
_Static_assert(sizeof(struct mem_stats) == 24, "mem_stats ABI");
_Static_assert(sizeof(struct net_frame) == 160, "net_frame ABI");
_Static_assert(offsetof(struct net_frame, data) == 32, "net_frame.data ABI");
_Static_assert(sizeof(struct net_stats) == 80, "net_stats ABI");
_Static_assert(sizeof(struct net_ping) == 16, "net_ping ABI");
_Static_assert(sizeof(struct audio_status) == 48, "audio_status ABI");
_Static_assert(sizeof(struct msg) == 12, "msg ABI");
_Static_assert(offsetof(struct msg, when) == 8, "msg.when ABI");
_Static_assert(sizeof(struct ipc_msg) == 88, "ipc_msg ABI");
_Static_assert(offsetof(struct ipc_msg, va) == 24, "ipc_msg.va ABI");
_Static_assert(offsetof(struct ipc_msg, str) == 40, "ipc_msg.str ABI");
_Static_assert(sizeof(struct fb_info) == 32, "fb_info ABI");
_Static_assert(sizeof(struct fb_rect) == 16, "fb_rect ABI");

#endif
