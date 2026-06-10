/* src/intf/arch/syscall.h — kernel-side syscall ABI.
 *
 * Defines every SYS_* number, the saved-register frame produced by
 * SYSCALL entry (src/impl/x86_64/cpu/syscall.asm), and the C dispatcher
 * the asm calls into. Numbers must stay in lockstep with the userspace
 * mirror in userspace/lib/syscall.h.
 *
 * Implementation: src/impl/kernel/arch/syscall.c.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* --- File / I/O surface ------------------------------------------------ */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_LSEEK   8
#define SYS_MMAP    9
#define SYS_READDIR 10
#define SYS_UNLINK  87

/* --- Process control -------------------------------------------------- */
#define SYS_YIELD   24
#define SYS_EXIT    60

/* --- Display + input -------------------------------------------------- */
#define SYS_FB_INFO    100
#define SYS_FB_MAP     101
#define SYS_FB_DAMAGE  108
#define SYS_KBD_POLL   102
#define SYS_GET_TICKS  103
#define SYS_EXEC       104
#define SYS_MSG_GET    105
#define SYS_MSG_PEEK   106
#define SYS_MOUSE_POS  107

/* --- Console (TTY) --------------------------------------------------- */
#define SYS_CON_WRITE      120
#define SYS_CON_CLEAR      121
#define SYS_SLEEP_TICKS    122
#define SYS_GET_PID        123

/* --- IPC / shmem / WM registry --------------------------------------- */
/* Used by userspace winman to compose windows owned by other processes.
 * The kernel mediates cross-PML4 page mapping; the WM protocol itself
 * lives entirely in userspace. */
#define SYS_IPC_SEND       130
#define SYS_IPC_RECV       131
#define SYS_SHMEM_SHARE    132
#define SYS_WM_REGISTER    133
#define SYS_WM_PID         134
#define SYS_TTY_DRAIN      135

/* --- Diagnostics ------------------------------------------------------ */
/* Consumed by userspace btop. */
#define SYS_PROC_LIST      140
#define SYS_MEM_STATS      141

/* --- Console alt-screen (single-level stack) ------------------------- */
/* Snapshot grid + cursor on push; restore on pop. Used by fullscreen
 * console apps so the shell's previous output reappears on exit. */
#define SYS_CON_PUSH       142
#define SYS_CON_POP        143

/* --- Process spawn --------------------------------------------------- */
/* Fire-and-forget: returns child pid, never waits. */
#define SYS_SPAWN          144

/* --- Power management ------------------------------------------------- */
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
