; kernel/arch/x86_64/interrupts/isr.asm , IDT entry stubs + common dispatcher.
;
; 48 vectors (0..31 = CPU exceptions, 32..47 = PIC-remapped IRQs), each
; entered via its own tiny stub so we know which vector fired and so
; error-code-pushing exceptions look the same as non-pushing ones to the
; C handler. The dummy-zero push for no-err vectors exists because x86
; couldn't agree with itself in 1985 about whether the CPU should push
; an error code, and we are still apologising for that decision.
;
; GS discipline: while ring 3 runs, GS.base is the user value and the CPU's
; cpu_local sits in KERNEL_GS_BASE. An interrupt taken from ring 3 therefore
; has to `swapgs` before any C code dereferences gs-relative state, and swap
; back on the way out. The saved CS tells us which case we are in , except in
; the narrow window a `swapgs; iretq` pair opens, where CPL is already 0 but
; GS is not yet kernel. Vectors that can land in that window take the
; paranoid entry below instead.
;
; Frame offsets are generated from struct interrupt_frame, not counted by
; hand , see kernel/arch/offsets.c.

%include "asm_offsets.inc"

extern isr_handler

; IA32_GS_BASE. Read directly by the paranoid entry, which cannot trust CS.
MSR_GS_BASE equ 0xC0000101

%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0          ; dummy error code
    push %1         ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push %1         ; CPU already pushed err code
    jmp isr_common
%endmacro

; NMI, #DF and #MC are delivered asynchronously or after the state that would
; normally answer "which ring were we in" is already gone. They get the
; GS-base test instead of the CS test.
%macro ISR_NOERR_PARANOID 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_paranoid_common
%endmacro

%macro ISR_ERR_PARANOID 1
global isr%1
isr%1:
    push %1
    jmp isr_paranoid_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR_PARANOID 2      ; NMI , IST2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR_PARANOID   8      ; #DF , IST1
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR_PARANOID 18     ; #MC
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47
ISR_NOERR 240       ; SMP work-queue wake IPI
ISR_NOERR 255       ; LAPIC spurious vector

; Save every general-purpose register. Interrupts are asynchronous , there is
; no calling convention to lean on, no caller-saved/callee-saved distinction
; we get to honour. Skipping any of these is a bug, full stop.
;
; Push order defines struct interrupt_frame. Both entry paths use it so the C
; handler sees one layout.
%macro PUSH_GPRS 0
  push rax
  push rcx
  push rdx
  push rbx
  push rbp
  push rsi
  push rdi
  push r8
  push r9
  push r10
  push r11
  push r12
  push r13
  push r14
  push r15
%endmacro

%macro POP_GPRS 0
  pop r15
  pop r14
  pop r13
  pop r12
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdi
  pop rsi
  pop rbp
  pop rbx
  pop rdx
  pop rcx
  pop rax
%endmacro

isr_common:
  PUSH_GPRS

  ; RSP is now the base of struct interrupt_frame. CS.RPL of 3 means we came
  ; from ring 3, so GS still holds the user value.
  test qword [rsp + INTERRUPT_FRAME_CS_OFF], 3
  jz .kernel_gs_ready
  swapgs
.kernel_gs_ready:

  mov rdi, rsp        ; SysV ABI: first arg = rdi = pointer to regs
  mov rbx, rsp        ; keep the real interrupt frame while aligning the call
  and rsp, -16        ; C expects a 16-byte-aligned stack before call
  cld                 ; required by SysV before calling C
  call isr_handler
  mov rsp, rbx

  POP_GPRS

  add rsp, INTERRUPT_EXIT_VEC_ERR_BYTES   ; discard vector + err code

  ; Same test against the CS the CPU will actually consume. The handler is
  ; allowed to have rewritten RIP (exception recovery does), but never CS.
  test qword [rsp + INTERRUPT_IRETQ_CS_OFF], 3
  jz .keep_kernel_gs
  swapgs
.keep_kernel_gs:
  iretq

; Paranoid entry: decide from GS.base itself rather than from CS.
;
; An NMI can arrive between the `swapgs` and the `iretq` of a return to ring 3.
; There CPL is 0 but GS.base is the user value, so a CS test concludes "kernel,
; no swap needed" and the handler runs on a user-controlled GS. Reading the MSR
; answers the question directly: cpu_local is a kernel image object and so is
; always in the high half of the address space, while the user GS base is not.
;
; This path leaves a wider swapgs/iretq window than isr_common does, which is
; safe precisely because it is the path a nested NMI would take: that NMI
; re-derives the GS state from the MSR and fixes it for itself.
isr_paranoid_common:
  PUSH_GPRS

  mov ecx, MSR_GS_BASE
  rdmsr                          ; edx:eax = GS base, GPRs already saved
  xor r11d, r11d                 ; 0 = GS was already kernel
  test edx, 0x80000000           ; bit 63 of the base: high half => kernel
  jnz .gs_is_kernel
  swapgs
  mov r11d, 1                    ; remember to undo it
.gs_is_kernel:

  mov rdi, rsp
  mov rbx, rsp
  and rsp, -16
  ; The flag has to outlive isr_handler, and every callee-saved register is a
  ; frame slot the handler may legitimately rewrite. Park it below the aligned
  ; call frame instead; nothing else uses this stack.
  sub rsp, 16
  mov [rsp], r11
  cld
  call isr_handler
  mov r11, [rsp]
  mov rsp, rbx

  ; Most vectors that come here panic and never return, but exception recovery
  ; can longjmp a fault back to its probe, so the return path has to be real.
  test r11d, r11d
  jz .keep_kernel_gs
  swapgs
.keep_kernel_gs:

  POP_GPRS
  add rsp, INTERRUPT_EXIT_VEC_ERR_BYTES
  iretq

; expose stub addresses as a C-visible table
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
  dq isr %+ i
%assign i i+1
%endrep
