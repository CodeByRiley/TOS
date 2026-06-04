#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_LSEEK   8
#define SYS_MMAP    9
#define SYS_READDIR 10
#define SYS_YIELD   24
#define SYS_EXIT    60
#define SYS_FB_INFO    100
#define SYS_FB_MAP     101
#define SYS_FB_DAMAGE  108
#define SYS_KBD_POLL   102
#define SYS_GET_TICKS  103
#define SYS_EXEC       104
#define SYS_MSG_GET    105
#define SYS_MSG_PEEK   106
#define SYS_MOUSE_POS  107
#define SYS_CON_WRITE  120
#define SYS_CON_CLEAR  121
#define SYS_SLEEP_TICKS 122
#define SYS_GET_PID     123

#define SYS_IPC_SEND    130
#define SYS_IPC_RECV    131
#define SYS_SHMEM_SHARE 132
#define SYS_WM_REGISTER 133
#define SYS_WM_PID      134
#define SYS_TTY_DRAIN   135

#define SYS_PROC_LIST   140
#define SYS_MEM_STATS   141
#define SYS_CON_PUSH    142
#define SYS_CON_POP     143
#define SYS_SPAWN       144

/* Mirror of kernel struct proc_info_user — byte-for-byte. */
#define PROC_NAME_MAX 16
struct proc_info {
    int      pid;
    int      parent_pid;
    int      state;
    int      _pad;
    uint64_t ticks_run;
    char     name[PROC_NAME_MAX];
};

/* Process states reported by SYS_PROC_LIST. Match enum task_state in
 * src/intf/sched/sched.h. */
#define PROC_STATE_RUNNING  0
#define PROC_STATE_BLOCKED  1
#define PROC_STATE_ZOMBIE   2
#define PROC_STATE_READY    3
#define PROC_STATE_DEAD     4
#define PROC_STATE_SLEEPING 5

struct mem_stats {
    uint64_t total_frames;
    uint64_t used_frames;
    uint64_t frame_size;
};

#define SYS_UNLINK     87

#define MSG_NONE        0
#define MSG_KEY_DOWN    1
#define MSG_KEY_UP      2
#define MSG_MOUSE_MOVE  3
#define MSG_MOUSE_DOWN  4
#define MSG_MOUSE_UP    5
#define MSG_TIMER       6
#define MSG_QUIT        7

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

struct msg {
    uint16_t type;
    uint16_t param;
    int16_t  x;
    int16_t  y;
    uint32_t when;
};

/* Cross-process control message — must match kernel src/intf/msg/msg.h.
 *
 * The WM-specific IPC_WM_* type codes live in lib/wm.h alongside the
 * winman client API. Generic / non-WM codes stay here. */
#define IPC_PEER_EXITED        0x180
#define IPC_USER_FIRST         0x200

struct ipc_msg {
    uint32_t type;
    uint32_t from_pid;
    int32_t  a, b, c, d;
    uint64_t va;
    uint32_t pitch;
    uint32_t flags;
    char     str[48];
};


#define SYS_SHUTDOWN   888
#define SYS_REBOOT     887

struct fb_info { uint64_t width, height, pitch, bpp; };


long syscall0(long n);
long syscall1(long n, long a);
long syscall2(long n, long a, long b);
long syscall3(long n, long a, long b, long c);
long syscall4(long n, long a, long b, long c, long d);
long syscall6(long n, long a, long b, long c, long d, long e, long f);

long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
long open(const char *path, int flags);
long close(int fd);
long lseek(int fd, long off, int whence);
void *mmap(size_t len);
long readdir(unsigned *index, char *buf, size_t n);
long unlink(const char *path);
void exit(int code);
long yield(void);
long msg_get(struct msg *out);
long msg_peek(struct msg *out);
long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons);

long  fb_info(struct fb_info *out);
void *fb_map(void);
long  fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
long  kbd_poll(int *pressed, uint16_t *key);
long  get_ticks(void);
long  exec(const char *path, char *const argv[]);

/* Fire-and-forget spawn: returns the child's pid (>0) immediately without
 * waiting for it to exit. Use for windowed apps so the shell stays free. */
long  spawn(const char *path, char *const argv[]);
long  sys_shutdown(int time, const char *reason);
long  sys_reboot(int time);

long  con_write(const char *buf, size_t n);
long  con_clear(void);

/* Save the current TTY screen + cursor onto a one-deep stack, then clear
 * the live screen. Pair with con_pop() to restore on exit. */
long  con_push(void);
long  con_pop(void);
long  sleep_ticks(unsigned long n);
long  get_pid(void);

/* IPC + shared memory primitives. ipc_send copies the message into the
 * target task's ring; from_pid in the receiver is filled by the kernel. */
long  ipc_send(int target_pid, const struct ipc_msg *m);
long  ipc_recv(struct ipc_msg *out);
long  shmem_share(int target_pid, uint64_t my_va, long npages,
                  uint64_t *out_target_va);

long  wm_register(void);
long  wm_pid(void);
long  tty_drain(char *buf, long max);

/* Snapshot the kernel task table. Writes up to `max` rows into `out`,
 * returns the number of rows filled (>=0) or -1 on bad arguments. */
long  proc_list(struct proc_info *out, long max);

/* Fill `out` with physical-memory accounting. Returns 0 on success. */
long  mem_stats(struct mem_stats *out);

/* High-level winman client API lives in lib/wm.h — include that header
 * instead of declaring winman entry points here. */

#endif
