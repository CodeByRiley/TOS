/* userspace/lib/syscall.h — userspace syscall interface.
 *
 * All system call numbers, kernel-shared structs, and the C wrappers
 * that userspace apps call instead of writing `syscall0..6` directly.
 *
 * Sections:
 *   - SYS_* numbers           — must match kernel/arch/syscall.h dispatch table.
 *   - proc_info / mem_stats   — mirrors of kernel structs returned by
 *                                SYS_PROC_LIST and SYS_MEM_STATS.
 *   - msg / ipc_msg           — input-event and cross-process message
 *                                layouts. ipc_msg must match
 *                                kernel/msg/msg.h byte-for-byte.
 *   - syscallN()              — raw register-passing trampolines.
 *   - libc-style wrappers     — typed helpers around the syscalls above.
 *
 * Higher-level winman client API lives in lib/wm.h, not here.
 */
#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* ---------------- System-call numbers ----------------------------------- */
#define SYS_READ    		 0
#define SYS_WRITE   		 1
#define SYS_OPEN    		 2
#define SYS_CLOSE   		 3
#define SYS_LSEEK   		 8
#define SYS_MMAP    		 9
#define SYS_READDIR 		 10
#define SYS_READDIR_PATH 11
#define SYS_CHDIR 			 12
#define SYS_GETCWD	 		 13


#define SYS_YIELD   	  24
#define SYS_EXIT    	  60
#define SYS_FB_INFO     100
#define SYS_FB_MAP      101
#define SYS_FB_DAMAGE   108
#define SYS_KBD_POLL    102
#define SYS_GET_TICKS   103
#define SYS_EXEC        104
#define SYS_MSG_GET     105
#define SYS_MSG_PEEK    106
#define SYS_MOUSE_POS   107
#define SYS_CON_WRITE   120
#define SYS_CON_CLEAR   121
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
#define SYS_KILL        145
#define SYS_CON_ZOOM    146

#define SYS_MKDIR      83
#define SYS_UNLINK     87
#define SYS_SHUTDOWN   888
#define SYS_REBOOT     887

/* ---------------- Process inspection (SYS_PROC_LIST) -------------------- */
/* Byte-for-byte mirror of kernel struct proc_info_user. */
#define PROC_NAME_MAX 16
struct proc_info {
    uint64_t ticks_run;

    int      pid;
    int      parent_pid;
    int      state;

    char     name[PROC_NAME_MAX];
};

_Static_assert(sizeof(struct proc_info) == 40,
               "proc_info must match kernel proc_info_user size");
_Static_assert(offsetof(struct proc_info, ticks_run) == 0,
               "proc_info.ticks_run offset must match kernel");
_Static_assert(offsetof(struct proc_info, pid) == 8,
               "proc_info.pid offset must match kernel");
_Static_assert(offsetof(struct proc_info, name) == 20,
               "proc_info.name offset must match kernel");

/* Process state codes — must match enum task_state in kernel/sched/sched.h. */
#define PROC_STATE_RUNNING  0
#define PROC_STATE_BLOCKED  1
#define PROC_STATE_ZOMBIE   2
#define PROC_STATE_READY    3
#define PROC_STATE_DEAD     4
#define PROC_STATE_SLEEPING 5

/* Physical memory accounting (SYS_MEM_STATS). */
struct mem_stats {
    uint64_t total_frames;
    uint64_t used_frames;
    uint64_t frame_size;
};

_Static_assert(sizeof(struct mem_stats) == 24,
               "mem_stats must match kernel mem_stats_user size");

/* ---------------- Input-event ring (SYS_MSG_GET/PEEK) ------------------- */
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
    uint16_t type;    /* MSG_*                 */
    uint16_t param;   /* keycode or btn mask   */
    int16_t  x;
    int16_t  y;
    uint32_t when;    /* ticks                 */
};

_Static_assert(sizeof(struct msg) == 12,
               "msg must match kernel struct msg size");

/* ---------------- Cross-process IPC ------------------------------------- */
/* Must match kernel kernel/msg/msg.h.
 *
 * Winman-specific IPC_WM_* type codes are declared in lib/wm.h alongside
 * the libwm client API. Codes 0x180..0x1FF are reserved for generic /
 * kernel-originated control messages; user-defined types start at 0x200. */
#define IPC_PEER_EXITED        0x180
#define IPC_USER_FIRST         0x200

struct ipc_msg {
    uint32_t type;
    uint32_t from_pid;   /* set by kernel on the receiver side */
    int32_t  a, b, c, d;
    uint64_t va;
    uint32_t pitch;
    uint32_t flags;
    char     str[48];
};

_Static_assert(sizeof(struct ipc_msg) == 88,
               "ipc_msg must match kernel struct ipc_msg size");

/* ---------------- Framebuffer info -------------------------------------- */
struct fb_info { uint64_t width, height, pitch, bpp; };

_Static_assert(sizeof(struct fb_info) == 32,
               "fb_info must match kernel fb_info size");

/* ---------------- Raw syscall trampolines ------------------------------- */
/* Assembly-defined wrappers that load arg registers and execute syscall.
 * Userspace code normally calls the typed wrappers below instead. */
long syscall0(long n);
long syscall1(long n, long a);
long syscall2(long n, long a, long b);
long syscall3(long n, long a, long b, long c);
long syscall4(long n, long a, long b, long c, long d);
long syscall6(long n, long a, long b, long c, long d, long e, long f);

/* ---------------- Typed libc-style wrappers ----------------------------- */
/* Standard POSIX-ish file / process surface. */
long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
long open(const char *path, int flags);
long close(int fd);
long lseek(int fd, long off, int whence);
void *mmap(size_t len);
long readdir(unsigned *index, char *buf, size_t n);
long readdir_path(const char *path, unsigned *index, char *buf, size_t n);
long mkdir_path(const char *path);
long unlink(const char *path);
void exit(int code);
long yield(void);
long chdir(const char *path);
char *getcwd(char *buf, size_t size);

/* Input + windowing. */
long msg_get(struct msg *out);
long msg_peek(struct msg *out);
long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons);
long  fb_info(struct fb_info *out);
void *fb_map(void);
long  fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
long  kbd_poll(int *pressed, uint16_t *key);
long  get_ticks(void);

/* Process control. exec() blocks until child exits; spawn() returns the
 * child's pid immediately so the caller (e.g., the shell) stays free. */
long  exec(const char *path, char *const argv[]);
long  spawn(const char *path, char *const argv[]);
long  kill(long pid, int signal);
long  sys_shutdown(int time, const char *reason);
long  sys_reboot(int time);

/* Console (TTY) surface. push/pop give fullscreen apps an alt-screen. */
long  con_write(const char *buf, size_t n);
long  con_clear(void);
long  con_push(void);
long  con_pop(void);
long  con_zoom(long delta);
long  sleep_ticks(unsigned long n);
long  get_pid(void);

/* IPC and shared memory primitives. The kernel fills in from_pid on the
 * receiver side; senders may leave it zero. */
long  ipc_send(int target_pid, const struct ipc_msg *m);
long  ipc_recv(struct ipc_msg *out);
long  shmem_share(int target_pid, uint64_t my_va, long npages,
                  uint64_t *out_target_va);

/* Winman registration / discovery. The pid of the WM is not cached because
 * winman is allowed to crash and respawn. */
long  wm_register(void);
long  wm_pid(void);
long  tty_drain(char *buf, long max);

/* Diagnostics. proc_list returns the number of rows filled; mem_stats
 * returns 0 on success. */
long  proc_list(struct proc_info *out, long max);
long  mem_stats(struct mem_stats *out);

#endif
