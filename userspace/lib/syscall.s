section .text

; long syscall0(long n)
global syscall0
syscall0:
    mov rax, rdi           ; n
    syscall
    ret

; long syscall1(long n, long a)
global syscall1
syscall1:
    mov rax, rdi
    mov rdi, rsi
    syscall
    ret

; long syscall2(long n, long a, long b)
global syscall2
syscall2:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    syscall
    ret

; long syscall3(long n, long a, long b, long c)
global syscall3
syscall3:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    syscall
    ret

; long syscall4(long n, long a, long b, long c, long d)
; SysV passes 4th arg in rcx, but `syscall` clobbers rcx with RIP — stage 4th in r10
; (which the kernel handler reads as a4).
global syscall4
syscall4:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov r10, r8
    syscall
    ret

; long syscall6(long n, long a, long b, long c, long d, long e, long f)
global syscall6
syscall6:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov r10, r8            ; SysV: 4th arg in rcx → kernel: 4th arg in r10 (syscall clobbers rcx)
    mov r8, r9
    mov r9, [rsp + 8]      ; 6th arg from stack
    syscall
    ret
