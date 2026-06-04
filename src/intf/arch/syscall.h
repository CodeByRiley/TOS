#ifndef SYSCALL_H
#define SYSCALL_H

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
#define SYS_CON_WRITE      120
#define SYS_CON_CLEAR      121
#define SYS_SLEEP_TICKS    122
#define SYS_GET_PID        123

/* IPC / shmem / window-manager registry — used by the userspace winman
 * to compose windows owned by other processes. The kernel mediates
 * cross-PML4 page mapping; everything else lives in winman itself. */
#define SYS_IPC_SEND       130
#define SYS_IPC_RECV       131
#define SYS_SHMEM_SHARE    132
#define SYS_WM_REGISTER    133
#define SYS_WM_PID         134
#define SYS_TTY_DRAIN      135

/* Introspection: process table + physical memory accounting. Consumed by
 * the userspace btop. */
#define SYS_PROC_LIST      140
#define SYS_MEM_STATS      141

/* Alt-screen save/restore for full-screen console apps. Saves current TTY
 * grid + cursor on push, restores on pop. One level deep. */
#define SYS_CON_PUSH       142
#define SYS_CON_POP        143

/* Fire-and-forget spawn: returns child pid, never waits for exit. */
#define SYS_SPAWN          144

#define SYS_UNLINK     87


#define SYS_SHUTDOWN   888
#define SYS_REBOOT     887

struct syscall_frame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, rbx, rbp, r10;
    uint64_t r9,  r8,  rcx, rdx;
    uint64_t rsi, rdi, rax;
};

void syscall_init(uint64_t kernel_stack_top);
long syscall_dispatch(struct syscall_frame *f);

#endif
