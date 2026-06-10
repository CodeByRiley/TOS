; src/impl/x86_64/cpu/syscall.asm — SYSCALL entry stub.
;
; Builds a 15-register frame on the kernel stack in the exact order
; `struct syscall_frame` (src/intf/arch/syscall.h) expects, calls into
; C, unwinds, sysrets.
;
; The push order below is load-bearing: each `push` maps to a struct
; field by position, so reorder one and the C dispatcher will silently
; read the wrong register from the wrong syscall arg. If you "clean
; this up" you will also be cleaning up production. Don't.
;
; kernel_rsp_top and user_rsp_save are re-staged on every context switch
; (sched.c::stage_for / capture_from) so per-task syscall stacks work.

global syscall_entry
global user_rsp_save
global kernel_rsp_top
extern syscall_dispatch

section .bss
align 8
user_rsp_save:  resq 1
kernel_rsp_top: resq 1

section .text
bits 64

; syscall instruction lands here (CPL=0 already from STAR)
; rax = syscall num, rdi/rsi/rdx/r10/r8/r9 = args
; rcx = saved user RIP, r11 = saved user RFLAGS
syscall_entry:
	mov [user_rsp_save], rsp
	mov rsp, [kernel_rsp_top]

	; build syscall_frame on stack (matches struct layout)
	push rax           ; rax (syscall num)
	push rdi
	push rsi
	push rdx           ; rdx
	push rcx           ; user RIP
	push r8
	push r9            ; r9
	push r10
	push rbp
	push rbx
	push r11           ; user RFLAGS
	push r12
	push r13
	push r14
	push r15

	mov rdi, rsp       ; pass &frame to dispatch
	sub rsp, 8         ; SysV: stack must be 16-byte aligned before call
	cld
	call syscall_dispatch
	add rsp, 8

	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop rbx
	pop rbp
	pop r10
	pop r9
	pop r8
	pop rcx
	pop rdx
	pop rsi
	pop rdi
	pop rax            ; return value (handler wrote here)

	mov rsp, [user_rsp_save]
	o64 sysret         ; rip=rcx, rflags=r11, CS+SS from STAR[63:48]
