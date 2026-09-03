; kernel/arch/x86_64/sched/switch.asm - context switch.
;
; void context_switch(u64 *old_rsp_ptr, u64 new_rsp,
;                     u64 new_cr3,
;                     void *old_fxstate, void *new_fxstate);
; Sys V AMD64: rdi=old_rsp_ptr, rsi=new_rsp, rdx=new_cr3,
;              rcx=old_fxstate, r8=new_fxstate.
; Both FPU buffers are mandatory, 16-byte-aligned task context storage.
; The caller stages CPU-local state with interrupts disabled before entry.

global context_switch
global context_enter
section .text
bits 64

context_switch:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15

  ; Every resumable task needs its x87/SSE state saved.
  fxsave64 [rcx]

  ; Save old RSP and switch to new kernel stack
  mov [rdi], rsp
  mov rsp, rsi

.restore:
  ; Switch address spaces if CR3 differs
  mov rax, cr3
  cmp rax, rdx
  je .same_cr3
  mov cr3, rdx

.same_cr3:
  ; The incoming task always supplies a valid FXSAVE64 image.
  fxrstor64 [r8]

  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; void context_enter(u64 new_rsp, u64 new_cr3, void *new_fxstate);
; One-way transfer for task exit: do not save registers, RSP or FPU state
; that will never be resumed. Incoming stack layout matches context_switch.
; rdi=new_rsp, rsi=new_cr3, rdx=new_fxstate; interrupts remain disabled.
context_enter:
  mov r8, rdx
  mov rdx, rsi
  mov rsp, rdi
  jmp context_switch.restore
