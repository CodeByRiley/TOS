; ============================================================================
;  DEAD CODE — kept around in case someone wants to reference the iretq-frame
;  setup or the context-stack trick. Neither `enter_user` nor `user_exit_jump`
;  is called from anywhere in src/ as of the scheduler-rewrite landing in
;  sched.c: user tasks are now spawned via task_spawn_user, and the first-run
;  trampoline (`user_task_trampoline` in sched.c) builds its own iretq frame
;  inline. This file used to be the one-shot ring-3 launcher before tasks
;  existed; it predates having an "exit" that meant something other than
;  "return to the kernel that called us".
;
;  Safe to delete. Hasn't been because nobody felt bold enough to git rm
;  hand-written assembly on a Friday.
; ============================================================================

global enter_user
global user_exit_jump
extern user_rsp_save

%define CTX_SIZE       80       ; 10 qwords
%define MAX_CTX_DEPTH  16

section .bss
align 16
context_stack: resb CTX_SIZE * MAX_CTX_DEPTH
context_idx:   resq 1

section .text
bits 64

; long enter_user(uint64_t entry, uint64_t user_rsp)
;   rdi = entry RIP, rsi = user RSP
;   pushes a save context, iretqs to ring 3.
;   user_exit_jump pops the matching context and returns to caller.
;   return value = exit code from sys_exit.
enter_user:
	; compute slot address: context_stack + idx * CTX_SIZE
	mov rax, [rel context_idx]
	mov rcx, CTX_SIZE
	mul rcx                     ; rax = idx * CTX_SIZE
	lea rcx, [rel context_stack]
	add rax, rcx                ; rax = &slot

	mov [rax + 0],  rbp
	mov [rax + 8],  rbx
	mov [rax + 16], r12
	mov [rax + 24], r13
	mov [rax + 32], r14
	mov [rax + 40], r15
	mov [rax + 48], rsp
	lea rdx, [rel .return_point]
	mov [rax + 56], rdx
	mov rdx, [rel user_rsp_save]
	mov [rax + 64], rdx
	pushfq
	pop rdx
	mov [rax + 72], rdx

	inc qword [rel context_idx]

	cli
	mov ax, 0x1B
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax

	push 0x1B                   ; SS
	push rsi                    ; user RSP
	push 0x202                  ; RFLAGS (IF=1)
	push 0x23                   ; CS
	push rdi                    ; user RIP
	iretq

.return_point:
	ret

; void user_exit_jump(long code)
;   pops top context, restores callee-saved + RSP, jumps to saved RIP
user_exit_jump:
	mov rax, rdi                ; rax = exit code (becomes enter_user return)

	dec qword [rel context_idx]

	mov rcx, [rel context_idx]
	mov rdx, CTX_SIZE
	push rax                    ; preserve exit code over mul
	mov rax, rcx
	mul rdx                     ; rax = idx * CTX_SIZE
	lea rcx, [rel context_stack]
	add rax, rcx                ; rax = &slot
	mov rcx, rax                ; move addr to rcx so we can recover code
	pop rax                     ; restore exit code

	mov rbp, [rcx + 0]
	mov rbx, [rcx + 8]
	mov r12, [rcx + 16]
	mov r13, [rcx + 24]
	mov r14, [rcx + 32]
	mov r15, [rcx + 40]
	mov rdx, [rcx + 64]
	mov [rel user_rsp_save], rdx
	mov rdx, [rcx + 72]
	mov r8, [rcx + 56]
	mov rsp, [rcx + 48]
	push rdx
	popfq
	jmp r8
