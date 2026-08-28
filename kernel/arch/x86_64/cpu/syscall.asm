; kernel/arch/x86_64/cpu/syscall.asm , SYSCALL entry stub.
;
; SYSCALL gives us CPL 0, a CS/SS from STAR, the user RIP in RCX and the user
; RFLAGS in R11 , and nothing else. In particular it does not switch stacks,
; so RSP still points at the ring-3 stack on the first instruction here. GS is
; how we find a stack we own: `swapgs` installs this CPU's cpu_local as the GS
; base, and CPU_LOCAL_KERNEL_RSP_TOP_OFF holds the current task's kernel stack.
;
; The frame this builds is the whole return contract. There is no global
; scratch and nothing for the scheduler to re-stage mid-syscall: a task that
; gets preempted inside a syscall carries its complete ring-3 state on its own
; kernel stack. cpu_local.user_rsp_save is touched only in the two-instruction
; window before the stack swap completes.
;
; Return is `iretq`, not `sysretq`. That is the correctness-first choice: the
; kernel stack stays live until the CPU itself performs the privilege change,
; and a future signal-delivery path gets one frame format to build. sysretq is
; faster but faults in ring 0 on a non-canonical RCX, so it needs its own
; validation and an IST to be safe. Measure before adding it.
;
; Layout is generated, not hand-copied , see kernel/arch/offsets.c.

%include "asm_offsets.inc"

global syscall_entry
extern syscall_dispatch

section .text
bits 64

; rax = syscall number, rdi/rsi/rdx/r10/r8/r9 = args,
; rcx = user RIP, r11 = user RFLAGS.
syscall_entry:
  swapgs

  ; Scratch, live only until RSP is a kernel stack.
  mov [gs:CPU_LOCAL_USER_RSP_SAVE_OFF], rsp
  mov rsp, [gs:CPU_LOCAL_KERNEL_RSP_TOP_OFF]

  ; iretq consumes RIP, CS, RFLAGS, RSP, SS in that order from low address up,
  ; so push them in reverse. SYSCALL masked RFLAGS through SFMASK before R11
  ; was handed to us; syscall_prepare_return() sanitises this copy again.
  push qword GDT_USER_DATA_RPL3      ; SS
  push qword [gs:CPU_LOCAL_USER_RSP_SAVE_OFF]
  push r11                           ; user RFLAGS
  push qword GDT_USER_CODE_RPL3      ; CS
  push rcx                           ; user RIP

  ; Saved registers. Push order defines struct syscall_frame , do not reorder
  ; without changing kernel/arch/syscall.h.
  push rax
  push rdi
  push rsi
  push rdx
  push rcx
  push r8
  push r9
  push r10
  push rbp
  push rbx
  push r11
  push r12
  push r13
  push r14
  push r15

  ; SYSCALL_FRAME_SIZE is a multiple of 16 and the stack top is 16-aligned, so
  ; RSP is already where SysV wants it before a call. No padding.
  mov rdi, rsp                       ; struct syscall_frame *
  cld                                ; SysV requires DF clear on entry to C
  call syscall_dispatch

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
  pop rax                            ; return value, written by the dispatcher

  ; RSP now points at the validated RIP/CS/RFLAGS/RSP/SS image. Keep these two
  ; adjacent: between them GS holds the user value while CPL is still 0, and
  ; only the NMI IST entry is prepared for that window.
  swapgs
  iretq
