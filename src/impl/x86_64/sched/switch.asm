; void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp, uint64_t new_cr3,
;                     void *old_fxstate, void *new_fxstate);
; Sys V AMD64: rdi=old_rsp_ptr, rsi=new_rsp, rdx=new_cr3,
;              rcx=old_fxstate, r8=new_fxstate.
;
; Saves callee-saved regs + x87/SSE state on current stack/buffer, stores rsp
; into *old_rsp_ptr, loads new_rsp, swaps CR3 if changed, restores x87/SSE
; state from new task's buffer, restores callee-saved, ret.
;
; The CR3 compare skips the (expensive) TLB-flushing mov when switching
; between two tasks that share an address space. fxsave/fxrstor are
; unconditional — XMM state must move with the task or memcpy-heavy threads
; will silently clobber each other's register file.

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

	; Save x87 + SSE state for the outgoing task. Buffer must be 16-aligned
	; (struct task layout enforces this).
	fxsave [rcx]

	; old rsp ptr = rsp
	; rsp = new rsp
	mov [rdi], rsp
	mov rsp, rsi

	mov rax, cr3
	cmp rax, rdx
	je .same_cr3
	mov cr3, rdx

.same_cr3:
	; Load x87 + SSE state for the incoming task.
	fxrstor [r8]

	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret
