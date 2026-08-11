/* userspace/lib/syscall_pe.c — syscall trampolines for PE builds.
 *
 * The PE counterpart of lib/syscall.s. ELF binaries get syscall0..6 from
 * that NASM file; mingw cannot assemble it, and more importantly the two
 * toolchains disagree about which registers hold arguments.
 *
 * mingw compiles to the Microsoft x64 convention — rcx, rdx, r8, r9 —
 * while the kernel reads arguments the SysV way: rdi, rsi, rdx, r10, r8,
 * r9, with the call number in rax. Rather than hand-writing the moves,
 * each stub below pins its operands to the registers the kernel expects
 * and lets the compiler emit whatever shuffle its own convention needs.
 *
 * syscall clobbers rcx with the return rip and r11 with the saved rflags,
 * which is why both appear in every clobber list. "memory" keeps the
 * compiler from caching across a call that can read or write anything.
 */
#include "syscall.h"

sysarg_t syscall0(sysarg_t n) {
    sysarg_t ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n)
                      : "rcx", "r11", "memory");
    return ret;
}

sysarg_t syscall1(sysarg_t n, sysarg_t a) {
    sysarg_t ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a)
                      : "rcx", "r11", "memory");
    return ret;
}

sysarg_t syscall2(sysarg_t n, sysarg_t a, sysarg_t b) {
    sysarg_t ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b)
                      : "rcx", "r11", "memory");
    return ret;
}

sysarg_t syscall3(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c) {
    sysarg_t ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return ret;
}

sysarg_t syscall4(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c,
                  sysarg_t d) {
    sysarg_t ret;
    register sysarg_t r10 __asm__("r10") = d;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "rcx", "r11", "memory");
    return ret;
}

sysarg_t syscall6(sysarg_t n, sysarg_t a, sysarg_t b, sysarg_t c,
                  sysarg_t d, sysarg_t e, sysarg_t f) {
    sysarg_t ret;
    register sysarg_t r10 __asm__("r10") = d;
    register sysarg_t r8  __asm__("r8")  = e;
    register sysarg_t r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}
