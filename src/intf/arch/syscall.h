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
#define SYS_EXIT    60
#define SYS_FB_INFO    100
#define SYS_FB_MAP     101
#define SYS_KBD_POLL   102
#define SYS_GET_TICKS  103
#define SYS_EXEC       104
#define SYS_UNLINK     87


#define SYS_SHUTDOWN   888
#define SYS_REBOOT     887

extern char  pending_exec_path[64];
extern int   pending_exec_set;

struct syscall_frame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, rbx, rbp, r10;
    uint64_t r9,  r8,  rcx, rdx;
    uint64_t rsi, rdi, rax;
};

void syscall_init(uint64_t kernel_stack_top);
long syscall_dispatch(struct syscall_frame *f);

#endif
