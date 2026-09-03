; kernel/arch/x86_64/boot/ap_trampoline.asm , AP startup trampoline.
;
; Copied verbatim to physical address 0x8000 by the BSP before sending
; SIPI. The AP wakes in 16-bit real mode at CS=0x0800, IP=0x0000 (linear
; 0x8000) and trampolines itself up to 64-bit long mode, then jumps to a
; higher-half handoff whose pointer is patched in at well-known offsets near
; the end of this blob. That handoff switches to the final kernel CR3 before
; installing the AP's heap-backed stack and entering C.
;
; Patch layout (offsets from 0x8000):
;   ap_pml4_phys  : dword , bootstrap PML4 physical address (< 4 GiB)
;   ap_cpu_id     : dword , CPU ID number
;   ap_stack_top  : qword , ABI-adjusted AP kernel stack
;   ap_handoff    : qword , virtual address of ap_long_mode_handoff()
;   ap_target_cr3 : qword , final kernel PML4 physical address
;
; Assembled with `nasm -f bin` so addresses are 0x8000-relative.

[bits 16]
org 0x8000

ap_trampoline_start:
  cli
  cld
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax

  lgdt [ap_gdt32_ptr]

  mov eax, cr0
  or  eax, 1                  ; CR0.PE
  mov cr0, eax

  jmp dword 0x08:ap_pmode_start

[bits 32]
ap_pmode_start:
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax

  ; Establish x87/SSE state assumed by the x86-64 ABI
  mov eax, cr0
  and eax, ~(1 << 2)          ; clear CR0.EM
  and eax, ~(1 << 3)          ; clear CR0.TS
  or  eax, 1 << 1             ; CR0.MP
  mov cr0, eax

  mov eax, cr4
  or  eax, (1 << 5) | (1 << 9) | (1 << 10) ; PAE | OSFXSR | OSXMMEXCPT
  mov cr4, eax

  fninit
  ldmxcsr [ap_mxcsr_default]

  mov eax, [ap_pml4_phys]
  mov cr3, eax

  mov ecx, 0xC0000080         ; IA32_EFER MSR
  rdmsr
  or  eax, 1 << 8             ; EFER.LME (long mode enable)
  or  eax, 1 << 11            ; EFER.NXE (no-execute enable, must match BSP)
  wrmsr

  mov eax, cr0
  or  eax, 1 << 31            ; CR0.PG
  mov cr0, eax

  lgdt [ap_gdt64_ptr]
  jmp 0x08:ap_lmode_start

[bits 64]
default abs
ap_lmode_start:
  xor eax, eax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov fs, ax
  mov gs, ax

  ; Keep all handoff state in registers. The linked higher-half stub changes
  ; CR3 before loading RSP, so the bootstrap root need not map the AP stack.
  mov rsi, [ap_target_cr3]
  mov rdx, [ap_stack_top]
  mov rax, [ap_handoff]
  mov edi, [ap_cpu_id]
  jmp rax

align 4
ap_mxcsr_default: dd 0x1F80

; ------------------------------ GDTs

align 8
ap_gdt32:
  dq 0
  dq 0x00CF9A000000FFFF       ; 32-bit code, ring 0, exec, 4 GiB
  dq 0x00CF92000000FFFF       ; 32-bit data, ring 0, write, 4 GiB
ap_gdt32_end:
ap_gdt32_ptr:
  dw ap_gdt32_end - ap_gdt32 - 1
  dd ap_gdt32

align 8
ap_gdt64:
  dq 0
  dq 0x00AF9A000000FFFF       ; 64-bit code, ring 0, exec
  dq 0x00CF92000000FFFF       ; 64-bit data, ring 0, write (L bit = 0)
ap_gdt64_end:
ap_gdt64_ptr:
  dw ap_gdt64_end - ap_gdt64 - 1
  dq ap_gdt64

; ------------------------------ patch slots
align 8
ap_pml4_phys:   dd 0
ap_cpu_id:      dd 0
ap_stack_top:   dq 0
ap_handoff:     dq 0
ap_target_cr3:  dq 0

ap_trampoline_end:
