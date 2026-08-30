; kernel/arch/x86_64/cpu/user_entry.asm , first entry into ring 3.
;
; A task that has never run userspace has no syscall frame to return through,
; so the very first transition has to be built by hand. This is the only place
; that does it. It is one-way: there is no saved kernel context, no nesting
; counter and no way back except a fault, an interrupt or a syscall, each of
; which has its own entry path.
;
; void arch_enter_user(u64 entry, u64 user_rsp, u64 arg);
;   rdi = entry, rsi = user_rsp, rdx = arg (delivered to userspace in RDI)
;
; FS and GS *selectors* are deliberately left alone. Their bases are kernel
; state now , FS_BASE is staged per task by the scheduler and GS is swapped
; below , and loading a selector into FS or GS can clear or leave undefined
; the hidden base the MSR just set. Long mode ignores DS/ES for addressing,
; but they are still loaded so a ring-3 `mov` from them sees a user selector
; rather than whatever the kernel was using.
;
; Selector values come from kernel/arch/offsets.c.

%include "asm_offsets.inc"

global arch_enter_user

; Bit 1 is architecturally always set; bit 9 is IF. Userspace starts with
; interrupts on and nothing else: DF, TF, NT and AC clear, IOPL 0.
USER_INITIAL_RFLAGS equ (1 << 1) | (1 << 9)

section .text
bits 64

arch_enter_user:
  cli

  ; Park the two values needed after the scrub below.
  mov rax, rdi                       ; user RIP
  mov r9,  rsi                       ; user RSP
  mov rdi, rdx                       ; userspace argument

  mov cx, GDT_USER_DATA_RPL3
  mov ds, cx
  mov es, cx

  ; Nothing else crosses the boundary. Every register ring 3 can read that
  ; is not part of the contract starts at zero rather than leaking whatever
  ; the kernel happened to leave there.
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

  ; iretq pops RIP, CS, RFLAGS, RSP, SS from low address up.
  push qword GDT_USER_DATA_RPL3      ; SS
  push r9                            ; RSP
  push qword USER_INITIAL_RFLAGS
  push qword GDT_USER_CODE_RPL3      ; CS
  push rax                           ; RIP

  xor eax, eax
  xor r9d, r9d

  ; GS is the last kernel-visible thing to go, and it goes immediately before
  ; the privilege change: between these two instructions CPL is still 0 while
  ; GS already holds the user value, which is why NMI runs on its own IST.
  swapgs
  iretq
