; kernel/arch/x86_64/cpu/user_entry.asm - First entry into ring 3.
;
; void arch_enter_user(u64 entry, u64 user_rsp, u64 arg);
;    rdi = entry, rsi = user_rsp, rdx = arg (delivered to userspace in RDI)

%include "asm_offsets.inc"

global arch_enter_user

; Bit 1 is architecturally always set; bit 9 is IF.
USER_INITIAL_RFLAGS equ (1 << 1) | (1 << 9)

section .text
bits 64

arch_enter_user:
  cli

  ; Park values needed after scrubbing
  mov rax, rdi                        ; user RIP
  mov r9,  rsi                        ; user RSP
  mov rdi, rdx                        ; userspace argument (arg0)

  mov cx, GDT_USER_DATA_RPL3
  mov ds, cx
  mov es, cx

  ; Scrub general-purpose registers to prevent kernel leaks
  xor ecx, ecx
  xor edx, edx
  xor esi, esi
  xor ebx, ebx
  xor ebp, ebp
  xor r8d,  r8d
  xor r10d, r10d
  xor r11d, r11d
  xor r12d, r12d
  xor r13d, r13d
  xor r14d, r14d
  xor r15d, r15d

  ; Scrub SSE/Vector registers (xmm0-xmm15) to prevent kernel leaks
  xorps xmm0,  xmm0
  xorps xmm1,  xmm1
  xorps xmm2,  xmm2
  xorps xmm3,  xmm3
  xorps xmm4,  xmm4
  xorps xmm5,  xmm5
  xorps xmm6,  xmm6
  xorps xmm7,  xmm7
  xorps xmm8,  xmm8
  xorps xmm9,  xmm9
  xorps xmm10, xmm10
  xorps xmm11, xmm11
  xorps xmm12, xmm12
  xorps xmm13, xmm13
  xorps xmm14, xmm14
  xorps xmm15, xmm15

  ; Final iretq stack frame, low to high: RIP, CS, RFLAGS, RSP, SS
  push qword GDT_USER_DATA_RPL3      ; SS
  push r9                             ; RSP
  push qword USER_INITIAL_RFLAGS      ; RFLAGS
  push qword GDT_USER_CODE_RPL3      ; CS
  push rax                            ; RIP

  xor eax, eax
  xor r9d, r9d

  ; Switch GS base to user before crossing privilege boundary
  swapgs
  iretq
