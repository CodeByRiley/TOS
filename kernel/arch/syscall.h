/* kernel/arch/syscall.h , kernel-side syscall ABI.
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

#include <utilities/types.h>
#include <stddef.h>
#include <stdint.h>

/* File / I/O surface
 *
 * Numbers follow Linux x86_64 wherever the call means the same thing, so
 * a future Linux personality doesn't need a translation table. TOS-only
 * calls take numbers Linux has not used at all.
 *
 * 13 (rt_sigaction) is deliberately left free.
 */
#define SYS_READ    		 0
#define SYS_WRITE   		 1
#define SYS_OPEN    		 2
#define SYS_CLOSE   		 3
#define SYS_STAT             4
#define SYS_FSTAT            5
#define SYS_POLL             7
#define SYS_LSEEK   		 8
#define SYS_MMAP    		 9
#define SYS_MPROTECT        10
#define SYS_MUNMAP          11
#define SYS_BRK             12
#define SYS_IOCTL           16
#define SYS_READV           19
#define SYS_WRITEV          20
#define SYS_NANOSLEEP       35
#define SYS_LINUX_GETPID    39

/* Linux x86_64 socket numbers. musl issues these directly from socket(),
 * bind(), sendto() and recvfrom(), so mirroring them is what lets ported
 * code and the musl socket API work without a TOS-specific shim. */
#define SYS_SOCKET          41
#define SYS_SENDTO          44
#define SYS_RECVFROM        45
#define SYS_BIND            49
#define SYS_FCNTL           72
#define SYS_GETCWD          79
#define SYS_CHDIR           80
#define SYS_MKDIR   		 83
#define SYS_RMDIR           84
#define SYS_UNLINK  		 87
#define SYS_GETTIMEOFDAY    96
#define SYS_READDIR        217   /* Linux getdents64 , that ABI only */
#define SYS_READDIR_INDEX  1125  /* TOS index-based directory walk */
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP     231
#define SYS_CLOCK_GETTIME  228
#define SYS_FSTATAT        262   /* Linux newfstatat                       */

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
  u64 size;
  u64 first_cluster;   /* FAT-specific; 0 for the root directory */
  u32 type;            /* STAT_TYPE_*                            */
  u32 attr;            /* raw FAT attribute byte                 */
};

_Static_assert(sizeof(struct stat_user) == 24,
               "stat_user must match the userspace mirror");

/* Process control */
#define SYS_YIELD   24
#define SYS_EXIT    60

/* Display + input */
#define SYS_FB_INFO    1000
#define SYS_FB_MAP     1001
#define SYS_FB_DAMAGE  1002
#define SYS_FB_PRESENT 1003
#define SYS_FB_REGISTER   1004
#define SYS_FB_UNREGISTER 1005
#define SYS_KBD_POLL   1006
#define SYS_GET_TICKS  1008
#define SYS_EXEC       1020
#define SYS_MSG_GET    1040
#define SYS_MSG_PEEK   1041
#define SYS_MOUSE_POS  1007
/* Console (TTY) */
#define SYS_CON_WRITE      1060
#define SYS_CON_CLEAR      1061
#define SYS_SLEEP_TICKS    1009
#define SYS_GET_PID        39
/* IPC / shmem / WM registry */
/* Used by userspace winman to compose windows owned by other processes.
 * The kernel mediates cross-PML4 page mapping; the WM protocol itself
 * lives entirely in userspace. */
#define SYS_IPC_SEND       1042
#define SYS_IPC_RECV       1043
#define SYS_SHMEM_SHARE    1044
#define SYS_SHMEM_UNSHARE  1045
#define SYS_WM_REGISTER    1065
#define SYS_WM_PID         1066
/* DRAIN and INJECT name their channel explicitly: winman mirrors several at
 * once and is itself on none of them. READ_INPUT has no index because stdin
 * is always the caller's own channel. */
#define SYS_TTY_DRAIN      1067  /* (idx, buf, max)  */
#define SYS_TTY_INJECT     1068  /* (idx, ch)        */
#define SYS_TTY_READ_INPUT 1069  /* (buf, max)       */
/* Console multiplexing. ALLOC claims a spare channel, FREE gives it back,
 * and SPAWN starts a program already bound to one , a plain SYS_SPAWN would
 * inherit winman's channel instead. */
#define SYS_TTY_ALLOC      1070  /* ()               */
#define SYS_TTY_FREE       1071  /* (idx)            */
#define SYS_TTY_SPAWN      1072  /* (path, argv, idx)*/
/* Diagnostics */
/* Consumed by userspace btop. */
#define SYS_PROC_LIST      1022
#define SYS_MEM_STATS      1023
/* Console alt-screen (single-level stack) */
/* Snapshot grid + cursor on push; restore on pop. Used by fullscreen
 * console apps so the shell's previous output reappears on exit. */
#define SYS_CON_PUSH       1062
#define SYS_CON_POP        1063
/* Process management */
/* Fire-and-forget: returns child pid, never waits. */
#define SYS_SPAWN          1021
#define SYS_KILL           62
#define SYS_CON_ZOOM       1064
/* PCM audio. The initial backend accepts signed 16-bit LE stereo. */
#define SYS_AUDIO_OPEN       1080
#define SYS_AUDIO_WRITE      1081
#define SYS_AUDIO_STATUS     1082
#define SYS_AUDIO_DRAIN      1083
#define SYS_AUDIO_CLOSE      1084
#define SYS_AUDIO_SET_VOLUME 1085
#define SYS_AUDIO_PAUSE      1086
#define SYS_AUDIO_RESUME     1087
/* Linux x86_64 TLS control. musl uses ARCH_SET_FS during startup. */
#define SYS_ARCH_PRCTL       158
#define ARCH_SET_FS       0x1002
#define ARCH_GET_FS       0x1003

#define AUDIO_FORMAT_S16_LE 1

struct audio_status_user {
  u32 available;
  u32 playing;
  u32 paused;
  u32 sample_rate;
  u32 channels;
  u32 format;
  u32 ring_capacity;
  u32 ring_queued;
  u32 device_queued;
  u32 underruns;
  u32 volume;
  int32_t  owner_pid;
};

_Static_assert(sizeof(struct audio_status_user) == 48,
               "audio status ABI must match userspace");

/* Thread management. */
#define SYS_THREAD_CREATE  1100
#define SYS_THREAD_EXIT    1101
#define SYS_THREAD_JOIN	   1102
#define SYS_FUTEX_WAIT     1103
#define SYS_FUTEX_WAKE	   1104
/* Power management */
#define SYS_SHUTDOWN   1120
#define SYS_REBOOT     1121
#define SYS_READDIR_PATH 1122
#define SYS_STAT_RAW     1123
#define SYS_FSTAT_RAW    1124
/* Network observation. Counters and the frame-capture ring in
 * kernel/net/netmon.h; no protocol state is reachable through these. */
#define SYS_NET_STATS    1140
#define SYS_NET_CAPTURE  1141
#define SYS_NET_PING     1142
/* Saved register frame produced by SYSCALL entry. Order matches the
 * pushes in syscall.asm , DO NOT reorder without updating both sides.
 * The C dispatcher reads syscall number from rax and args from rdi/rsi/
 * rdx/r10/r8/r9 (SysV minus rcx, which holds the saved RIP). */
struct syscall_frame {
    u64 r15, r14, r13, r12;
    u64 r11, rbx, rbp, r10;
    u64 r9,  r8,  rcx, rdx;
    u64 rsi, rdi, rax;
    u64 rip, cs, rflags, rsp, ss;
};

_Static_assert(sizeof(struct syscall_frame) == 20 * sizeof(u64),
               "syscall_frame must match syscall.asm pushes");
_Static_assert(offsetof(struct syscall_frame, r10) == 7 * sizeof(u64),
               "syscall_frame.r10 must match syscall arg4 slot");
_Static_assert(offsetof(struct syscall_frame, rax) == 14 * sizeof(u64),
               "syscall_frame.rax must be the saved return-value slot");
_Static_assert(offsetof(struct syscall_frame, rip) == 15 * sizeof(u64),
               "syscall_frame.rip must begin the iretq frame");
_Static_assert(offsetof(struct syscall_frame, rsp) == 18 * sizeof(u64),
               "syscall_frame.rsp must match the iretq user-rsp slot");
_Static_assert(offsetof(struct syscall_frame, ss) == 19 * sizeof(u64),
               "syscall_frame.ss must end the iretq frame");

/* Program this CPU's LSTAR / STAR / SFMASK, enable SCE, and stage its initial
 * ring-3 entry stack. Invoke once on the BSP and once from every AP. */
void syscall_init_this_cpu(u64 kernel_stack_top);

/* Validate and sanitize the ring-3 return portion of a completed frame.
 * Returns zero when iretq may consume it. */
int syscall_prepare_return(struct syscall_frame *f);

/* Main dispatcher invoked from syscall.asm. Returns the long value that
 * will be loaded into rax for the userspace caller. */
long syscall_dispatch(struct syscall_frame *f);

#endif
