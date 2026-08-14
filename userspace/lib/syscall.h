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

/* ---------------- System-call numbers -----------------------------------
 *
 * Linux x86_64 numbers wherever the call means the same thing; TOS-only
 * calls take numbers Linux has not used. 13 (rt_sigaction) is deliberately
 * left free. */
#define SYS_READ    		     	 0
#define SYS_WRITE   		     	 1
#define SYS_OPEN    		     	 2
#define SYS_CLOSE   		     	 3
#define SYS_STAT             	 4
#define SYS_FSTAT            	 5
#define SYS_POLL             	 7
#define SYS_LSEEK   		     	 8
#define SYS_MMAP    		     	 9
#define SYS_MPROTECT         	 10
#define SYS_MUNMAP           	 11
#define SYS_BRK              	 12
#define SYS_IOCTL            	 16
#define SYS_READV            	 19
#define SYS_WRITEV           	 20
#define SYS_NANOSLEEP        	 35
#define SYS_LINUX_GETPID     	 39
#define SYS_FCNTL            	 72
#define SYS_GETCWD           	 79
#define SYS_CHDIR            	 80
#define SYS_RMDIR            	 84
#define SYS_READDIR          	 217
#define SYS_SET_TID_ADDRESS  	 218
#define SYS_CLOCK_GETTIME    	 228
#define SYS_EXIT_GROUP       	 231
#define SYS_FSTATAT          	 262

#define SYS_YIELD   	  	   	 24
#define SYS_EXIT    	  	   	 60
#define SYS_FB_INFO     	   	 100
#define SYS_FB_MAP      	   	 101
#define SYS_FB_DAMAGE   	   	 108
#define SYS_FB_PRESENT       	 109
#define SYS_FB_REGISTER      	 110
#define SYS_FB_UNREGISTER    	 111
#define SYS_KBD_POLL    	   	 102
#define SYS_GET_TICKS   	   	 103
#define SYS_EXEC        	   	 104
#define SYS_MSG_GET     	   	 105
#define SYS_MSG_PEEK    	   	 106
#define SYS_MOUSE_POS   	   	 107
#define SYS_CON_WRITE   	   	 120
#define SYS_CON_CLEAR   	   	 121
#define SYS_SLEEP_TICKS 	   	 122
#define SYS_GET_PID     	   	 123
#define SYS_IPC_SEND         	 130
#define SYS_IPC_RECV         	 131
#define SYS_SHMEM_SHARE      	 132
#define SYS_SHMEM_UNSHARE    	 133
#define SYS_WM_REGISTER      	 134
#define SYS_WM_PID           	 135
#define SYS_TTY_DRAIN        	 136

#define SYS_PROC_LIST        	 140
#define SYS_MEM_STATS        	 141
#define SYS_CON_PUSH         	 142
#define SYS_CON_POP          	 143
#define SYS_SPAWN            	 144
#define SYS_KILL             	 145
#define SYS_CON_ZOOM         	 146

#define SYS_AUDIO_OPEN       	 147
#define SYS_AUDIO_WRITE      	 148
#define SYS_AUDIO_STATUS     	 149
#define SYS_AUDIO_DRAIN      	 150
#define SYS_AUDIO_CLOSE      	 151
#define SYS_AUDIO_SET_VOLUME 	 152
#define SYS_AUDIO_PAUSE      	 153
#define SYS_AUDIO_RESUME     	 154

#define SYS_ARCH_PRCTL          158
#define ARCH_SET_FS         0x1002
#define ARCH_GET_FS         0x1003

#define AUDIO_FORMAT_S16_LE    1
#define AUDIO_CHANNELS_STEREO  2

#define AUDIO_ERR_NO_DEVICE    (-1)
#define AUDIO_ERR_BUSY         (-2)
#define AUDIO_ERR_INVALID      (-3)
#define AUDIO_ERR_NOT_OWNER    (-4)

#define SYS_THREAD_CREATE  		 200
#define SYS_THREAD_EXIT    		 201
#define SYS_THREAD_JOIN	   		 202
#define SYS_FUTEX_WAIT     		 203
#define SYS_FUTEX_WAKE	   		 204

#define SYS_MKDIR          		 83
#define SYS_UNLINK         		 87
#define SYS_SHUTDOWN       		 888
#define SYS_REBOOT         		 887
#define SYS_READDIR_PATH     	 889
#define SYS_STAT_RAW         	 890
#define SYS_FSTAT_RAW        	 891

/* ---------------- mmap / mprotect --------------------------------------
 *
 * Linux PROT_ and MAP_ values. Two deliberate deviations from Linux:
 *
 *   - MAP_FIXED FAILS if any page in the range is already mapped, rather
 *     than replacing it. A loader probing its preferred image base gets
 *     "taken, go relocate" instead of a silently clobbered mapping.
 *   - PROT_NONE is rejected. Mappings reserve virtual space but are not
 *     backed until accessed, but there is no use-case for mapping a region
 *     as completely inaccessible, so PROT_NONE is still an error.
 *
 * Mappings are always anonymous, private, and zero-filled. There is no
 * file-backed mapping: map the range, then read() into it. */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS

#define MAP_FAILED      ((void *)-1)

/* ---------------- File metadata (SYS_STAT / SYS_FSTAT) ------------------ */
/* Byte-for-byte mirror of kernel struct stat_user. libc's POSIX struct
 * stat is built from this in lib/stat.c. */
#define STAT_TYPE_FILE 0
#define STAT_TYPE_DIR  1

struct stat_user {
    uint64_t size;
    uint64_t first_cluster;
    uint32_t type;
    uint32_t attr;
};

_Static_assert(sizeof(struct stat_user) == 24,
               "stat_user must match kernel struct stat_user size");

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
#define PROC_STATE_LOADING  6

/* Physical memory accounting (SYS_MEM_STATS). */
struct mem_stats {
    uint64_t total_frames;
    uint64_t used_frames;
    uint64_t frame_size;
};

_Static_assert(sizeof(struct mem_stats) == 24,
               "mem_stats must match kernel mem_stats_user size");

/* PCM output status. ring_queued is data waiting in the kernel queue;
 * device_queued has already reached DMA but has not finished playing. */
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
    int32_t  owner_pid;
};

_Static_assert(sizeof(struct audio_status) == 48,
               "audio_status must match the kernel ABI");

/* ---------------- Input-event ring (SYS_MSG_GET/PEEK) ------------------- */
#define MSG_NONE        0
#define MSG_KEY_DOWN    1
#define MSG_KEY_UP      2
#define MSG_MOUSE_MOVE  3
#define MSG_MOUSE_DOWN  4
#define MSG_MOUSE_UP    5
#define MSG_TIMER       6
#define MSG_QUIT        7

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04
#define MOUSE_BTN_FORWARD 0x08
#define MOUSE_BTN_BACK    0x10


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
#define IPC_PEER_EXITED				 0x180
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

#define FB_PRESENT_MAX_RECTS 16

struct fb_rect {
    uint32_t x, y, w, h;
};

_Static_assert(sizeof(struct fb_rect) == 16,
               "fb_rect must match kernel fb_rect size");

/* ---------------- Raw syscall trampolines -------------------------------
 *
 * Everything crossing the syscall boundary is sysarg_t, never `long`.
 * The ELF toolchain is LP64 so the two are the same width there, but the
 * PE variants are built by mingw, which is LLP64: `long` is four bytes.
 * A pointer passed as long would reach the kernel truncated to its low 32
 * bits — for a PE image based at 0x140000000, an address 4 GiB from the
 * one intended. long long is 64-bit under both models.
 *
 * ELF builds get these from lib/syscall.s; PE builds from lib/syscall_pe.c.
 * Userspace code normally calls the typed wrappers below instead. */
typedef long long sysarg_t;

sysarg_t syscall0(sysarg_t n);
sysarg_t syscall1(sysarg_t n, sysarg_t a);
sysarg_t syscall2(sysarg_t n, sysarg_t a, sysarg_t b);
sysarg_t syscall3(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c);
sysarg_t syscall4(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c, sysarg_t d);
sysarg_t syscall6(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c, sysarg_t d,
                  sysarg_t e, sysarg_t f);

/* Pointer -> syscall argument, at full width under both data models. */
#define SYSPTR(p) ((sysarg_t)(uintptr_t)(p))

/* ---------------- Typed libc-style wrappers ----------------------------- */
/* Standard POSIX-ish file / process surface. */
long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
long open(const char *path, int flags);
long close(int fd);
long lseek(int fd, long off, int whence);
long readdir(unsigned *index, char *buf, size_t n);
long readdir_path(const char *path, unsigned *index, char *buf, size_t n);
long mkdir_path(const char *path);
long rmdir_path(const char *path);
long unlink(const char *path);
void exit(int code);
long yield(void);
long chdir(const char *path);
char *getcwd(char *buf, size_t size);

/* Raw metadata straight from the kernel. POSIX stat()/fstat() in
 * <sys/stat.h> wrap these. */
long stat_raw(const char *path, struct stat_user *out);
long fstat_raw(int fd, struct stat_user *out);

/* Anonymous, private, zero-filled, demand-paged.
 *
 * addr is a request, honoured only with MAP_FIXED — and MAP_FIXED fails
 * rather than replacing an existing mapping. Returns MAP_FAILED on error.
 *
 * Physical memory is NOT allocated immediately. The kernel only reserves
 * the virtual address space. Physical RAM is allocated one page at a time
 * by the page fault handler when the program actually reads or writes to it.
 */
void *mmap(void *addr, size_t len, int prot, int flags);
int   mprotect(void *addr, size_t len, int prot);
int   munmap(void *addr, size_t len);

/* Input + windowing. */
long msg_get(struct msg *out);
long msg_peek(struct msg *out);
long mouse_pos(int32_t *x, int32_t *y, uint8_t *buttons);
long  fb_info(struct fb_info *out);
void *fb_map(void);
long  fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
long  fb_present(const void *pixels, uint32_t pitch,
                 const struct fb_rect *rects, uint32_t rect_count);
long  fb_register(const void *pixels, uint32_t pitch);
long  fb_unregister(void);
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

/* Audio output. audio_write is non-blocking and can return a short count or
 * zero when the ring is full. Input must be frame-aligned S16-LE stereo PCM.
 * audio_drain blocks until queued samples finish; audio_close discards them. */
long audio_open(uint32_t sample_rate, uint32_t channels, uint32_t format);
long audio_write(const void *pcm, size_t bytes);
long audio_status(struct audio_status *out);
long audio_drain(void);
long audio_close(void);
long audio_set_volume(int percent);
long audio_pause(void);
long audio_resume(void);

/* IPC and shared memory primitives. The kernel fills in from_pid on the
 * receiver side; senders may leave it zero. */
long  ipc_send(int target_pid, const struct ipc_msg *m);
long  ipc_recv(struct ipc_msg *out);
long  shmem_share(int target_pid, uint64_t in_va, long npages,
                  uint64_t *out_target_va);
long  shmem_unshare(int target_pid, uint64_t in_va, long npages);

/* Winman registration / discovery. The pid of the WM is not cached because
 * winman is allowed to crash and respawn. */
long  wm_register(void);
long  wm_pid(void);
long  tty_drain(char *buf, long max);

/* Diagnostics. proc_list returns the number of rows filled; mem_stats
 * returns 0 on success. */
long  proc_list(struct proc_info *out, long max);
long  mem_stats(struct mem_stats *out);

/* Threading */
long  thread_create(void *(*entry)(void *), void *stack, void *arg);
long  thread_exit(void);
long  thread_join(long tid);
long  futex_wait(uint32_t *addr, uint32_t expected);
long  futex_wake(uint32_t *addr);

#endif
