; kernel/arch/x86_64/sched/switch.asm , context switch.
;
; void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp,
;                     uint64_t new_cr3,
;                     void *old_fxstate, void *new_fxstate);
; Sys V AMD64: rdi=old_rsp_ptr, rsi=new_rsp, rdx=new_cr3,
;              rcx=old_fxstate, r8=new_fxstate.
;
; Saves callee-saved regs + x87/SSE state on current stack/buffer,
; stores rsp into *old_rsp_ptr, loads new_rsp, swaps CR3 if changed,
; restores x87/SSE state from new task's buffer, restores callee-saved,
; rets.
;
; The CR3 compare skips the (expensive) TLB-flushing mov when switching
; between two tasks that share an address space. fxsave/fxrstor are
; unconditional , XMM state must move with the task or memcpy-heavy
; threads will silently clobber each other's register file.
;
; RFLAGS is deliberately NOT saved or restored here. There is no missing
; pushfq/popfq pair; IF is a caller invariant instead:
;
;   - context_switch is entered with IF clear and returns with IF clear. Every
;     call site is inside a sched.c irq_save()/irq_restore() region.
;   - The interrupted C caller's own saved RFLAGS is a local on that task's
;     kernel stack, so it travels with the task for free. When the task is
;     scheduled again it returns into its own irq_restore(), which puts back
;     the IF state that task had.
;   - Anything with no such caller states its interrupt policy explicitly:
;     kthread_trampoline does `sti`, idle_thread runs `sti; hlt` atomically,
;     and a first entry into ring 3 gets RFLAGS 0x202 from arch_enter_user.
;
; Restoring RFLAGS from inside this routine would break that: it would enable
; interrupts at the instant the incoming task's stack has been installed but
; before its caller has finished re-establishing scheduler, TSS and CPU-local
; state. Per-CPU preemption counting is the right tool when SMP scheduling
; needs finer control, not treating RFLAGS as task context.

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
