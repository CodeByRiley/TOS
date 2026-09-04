/* kernel/arch/syscall.h , kernel-side syscall ABI.
 *
 * Imports every SYS_* number from syscalls.def, and defines the saved-register
 * frame produced by SYSCALL entry plus the C dispatcher the asm calls into.
 *
 * Implementation: kernel/arch/syscall.c.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <utilities/types.h>
#include <stddef.h>
#include <stdint.h>
#include <arch/syscall_abi.h>

/* Numbers follow Linux x86_64 wherever the call means the same thing. The
 * registry also records whether libtos exposes a call or only musl issues it
 * directly. Both sides include the same table, so a number cannot drift. */
enum syscall_number {
#define SYSCALL_BOTH(name, number) SYS_##name = number,
#define SYSCALL_KERNEL(name, number) SYS_##name = number,
#define SYSCALL_ALIAS_BOTH(name, target) SYS_##name = SYS_##target,
#include <arch/syscalls.def>
#undef SYSCALL_ALIAS_BOTH
#undef SYSCALL_KERNEL
#undef SYSCALL_BOTH
};

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

/* Linux x86_64 TLS control. musl uses ARCH_SET_FS during startup. */
#define ARCH_SET_FS          0x1002
#define ARCH_GET_FS          0x1003

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
