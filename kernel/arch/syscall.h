/* kernel/arch/syscall.h — kernel-side syscall ABI.
 *
 * Defines every SYS_* number, the saved-register frame produced by
 * SYSCALL entry (kernel/arch/x86_64/cpu/syscall.asm), and the C dispatcher
 * the asm calls into. Numbers must stay in lockstep with the userspace
 * mirror in userspace/lib/syscall.h.
 *
 * Implementation: kernel/arch/syscall.c.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* File / I/O surface
 *
 * Numbers follow Linux x86_64 wherever the call means the same thing, so
 * a future Linux personality doesn't need a translation table. TOS-only
 * calls take numbers Linux has not used at all.
 *
 * 12 (brk) and 13 (rt_sigaction) are deliberately left free.
 */
#define SYS_READ    		 0
#define SYS_WRITE   		 1
#define SYS_OPEN    		 2
#define SYS_CLOSE   		 3
#define SYS_STAT             4
#define SYS_FSTAT            5
#define SYS_LSEEK   		 8
#define SYS_MMAP    		 9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_MKDIR   		 83
#define SYS_RMDIR           84
#define SYS_UNLINK  		 87
#define SYS_READDIR        217   /* Linux getdents64 slot                  */
#define SYS_READDIR_PATH   218   /* TOS extension: enumerate by path       */

/* mmap / mprotect
 *
 * PROT_* and MAP_* use the Linux values. Deviations from Linux, both
 * deliberate:
 *
 *   - MAP_FIXED fails with -1 if any page in the range is already mapped
 *     instead of silently replacing it. A loader trying its preferred
 *     ImageBase wants "taken, go relocate", not a clobbered mapping.
 *   - PROT_NONE is rejected. Every mapping here is backed by a real frame
 *     at map time, so there is nothing to represent a reserved-but-absent
 *     page; guard pages need a reservation concept the VMM doesn't have.
 */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

/* stat
 * Compact kernel-side metadata. Userspace libc expands this into the
 * POSIX struct stat; keeping the ABI struct small means adding a POSIX
 * field later doesn't change the syscall boundary. */
#define STAT_TYPE_FILE 0
#define STAT_TYPE_DIR  1

struct stat_user {
  uint64_t size;
  uint64_t first_cluster;   /* FAT-specific; 0 for the root directory */
  uint32_t type;            /* STAT_TYPE_*                            */
  uint32_t attr;            /* raw FAT attribute byte                 */
};

_Static_assert(sizeof(struct stat_user) == 24,
               "stat_user must match the userspace mirror");

/* Process control */
#define SYS_YIELD   24
#define SYS_EXIT    60

/* Display + input */
#define SYS_FB_INFO    100
#define SYS_FB_MAP     101
#define SYS_FB_DAMAGE  108
#define SYS_FB_PRESENT 109
#define SYS_FB_REGISTER   110
#define SYS_FB_UNREGISTER 111
#define SYS_KBD_POLL   102
#define SYS_GET_TICKS  103
#define SYS_EXEC       104
#define SYS_MSG_GET    105
#define SYS_MSG_PEEK   106
#define SYS_MOUSE_POS  107

/* Console (TTY) */
#define SYS_CON_WRITE      120
#define SYS_CON_CLEAR      121
#define SYS_SLEEP_TICKS    122
#define SYS_GET_PID        123

/* IPC / shmem / WM registry */
/* Used by userspace winman to compose windows owned by other processes.
 * The kernel mediates cross-PML4 page mapping; the WM protocol itself
 * lives entirely in userspace. */
#define SYS_IPC_SEND       130
#define SYS_IPC_RECV       131
#define SYS_SHMEM_SHARE    132
#define SYS_SHMEM_UNSHARE  133
#define SYS_WM_REGISTER    134
#define SYS_WM_PID         135
#define SYS_TTY_DRAIN      136

/* Diagnostics */
/* Consumed by userspace btop. */
#define SYS_PROC_LIST      140
#define SYS_MEM_STATS      141

/* Console alt-screen (single-level stack) */
/* Snapshot grid + cursor on push; restore on pop. Used by fullscreen
 * console apps so the shell's previous output reappears on exit. */
#define SYS_CON_PUSH       142
#define SYS_CON_POP        143

/* Process management */
/* Fire-and-forget: returns child pid, never waits. */
#define SYS_SPAWN          144
#define SYS_KILL           145
#define SYS_CON_ZOOM       146

/* PCM audio. The initial backend accepts signed 16-bit LE stereo. */
#define SYS_AUDIO_OPEN       147
#define SYS_AUDIO_WRITE      148
#define SYS_AUDIO_STATUS     149
#define SYS_AUDIO_DRAIN      150
#define SYS_AUDIO_CLOSE      151
#define SYS_AUDIO_SET_VOLUME 152
#define SYS_AUDIO_PAUSE      153
#define SYS_AUDIO_RESUME     154

#define AUDIO_FORMAT_S16_LE 1

struct audio_status_user {
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

_Static_assert(sizeof(struct audio_status_user) == 48,
               "audio status ABI must match userspace");

/* Thread management. */
#define SYS_THREAD_CREATE  200
#define SYS_THREAD_EXIT    201
#define SYS_THREAD_JOIN	   202
#define SYS_FUTEX_WAIT     203
#define SYS_FUTEX_WAKE	   204

/* Power management */
#define SYS_SHUTDOWN   888
#define SYS_REBOOT     887

/* Saved register frame produced by SYSCALL entry. Order matches the
 * pushes in syscall.asm — DO NOT reorder without updating both sides.
 * The C dispatcher reads syscall number from rax and args from rdi/rsi/
 * rdx/r10/r8/r9 (SysV minus rcx, which holds the saved RIP). */
struct syscall_frame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, rbx, rbp, r10;
    uint64_t r9,  r8,  rcx, rdx;
    uint64_t rsi, rdi, rax;
};

_Static_assert(sizeof(struct syscall_frame) == 15 * sizeof(uint64_t),
               "syscall_frame must match syscall.asm pushes");
_Static_assert(offsetof(struct syscall_frame, r10) == 7 * sizeof(uint64_t),
               "syscall_frame.r10 must match syscall arg4 slot");
_Static_assert(offsetof(struct syscall_frame, rax) == 14 * sizeof(uint64_t),
               "syscall_frame.rax must be the saved return-value slot");

/* Program LSTAR / STAR / SFMASK and enable SCE. Called once at BSP boot
 * with the kernel stack to load into kernel_rsp_top. */
void syscall_init(uint64_t kernel_stack_top);

/* Main dispatcher invoked from syscall.asm. Returns the long value that
 * will be loaded into rax for the userspace caller. */
long syscall_dispatch(struct syscall_frame *f);

#endif
