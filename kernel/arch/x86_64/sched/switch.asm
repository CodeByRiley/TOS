; kernel/arch/x86_64/sched/switch.asm - context switch.
;
; void context_switch(u64 *old_rsp_ptr, u64 new_rsp,
;                     u64 new_cr3,
;                     void *old_fxstate, void *new_fxstate);
; Sys V AMD64: rdi=old_rsp_ptr, rsi=new_rsp, rdx=new_cr3,
;              rcx=old_fxstate, r8=new_fxstate.

global context_switch
section .text
bits 64

context_switch:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15

  ; Save x87 + SSE state for outgoing task if buffer is provided
  test rcx, rcx
  jz .skip_save
  fxsave64 [rcx]

.skip_save:
  ; Save old RSP and switch to new kernel stack
  mov [rdi], rsp
  mov rsp, rsi

  ; Switch address spaces if CR3 differs
  mov rax, cr3
  cmp rax, rdx
  je .same_cr3
  mov cr3, rdx

.same_cr3:
  ; Restore x87 + SSE state for incoming task if buffer is provided
  test r8, r8
  jz .skip_restore
  fxrstor64 [r8]

.skip_restore:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret
