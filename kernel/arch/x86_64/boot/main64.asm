; kernel/arch/x86_64/boot/main64.asm - 64-bit landing pad
extern kernel_main
extern ap_main

section .boot.text progbits alloc exec nowrite align=16
bits 64

global long_mode_start

; Low 64-bit trampoline.
; rdi: mb2 info pointer
long_mode_start:
  cli
  cld

  ; rdi (mb2 pointer) passes through untouched to kernel_main.
  ; Bootstrap PML4 maps the high-half .text, so high-half serial routines are callable.
  mov rsi, msg_lm_entered
  mov rax, serial_putln
  call rax

  mov rax, high_half_start
  jmp rax

section .boot.rodata progbits alloc noexec nowrite align=8
msg_lm_entered: db "[BOOT(64)] long mode entered", 0


section .text progbits alloc exec nowrite align=16
bits 64

global high_half_start
global ap_long_mode_handoff

; Stackless AP address-space handoff.
; rdi: cpu_id, rsi: target CR3, rdx: target kernel RSP
ap_long_mode_handoff:
  cli                 ; No valid IDT exists for this CPU yet
  cld                 ; System V ABI requirement (DF = 0)
  mov cr3, rsi
  mov rsp, rdx
  xor ebp, ebp        ; Unwind terminator for ap_main's backtraces
  jmp ap_main

; High-half kernel entry.
; rdi: mb2 info pointer
high_half_start:
  cli
  cld

  push rdi

  mov rsi, msg_hi_started
  call serial_putln

  ; Zero segment registers (DS, ES, SS, FS, GS are largely ignored in 64-bit mode)
  xor eax, eax
  mov ss, ax
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax

  mov rsi, msg_lm_segs
  call serial_putln

  mov rsi, msg_lm_call_kernel
  call serial_putln

  pop rdi
  call kernel_main

.hang:
  cli
.hang_loop:
  hlt
  jmp .hang_loop


; Send one byte over COM1
; al: char to send (clobbers ah, dx)
serial_putc:
  mov ah, al          ; Stash char
.wait:
  pause               ; HT / spin-loop hint
  mov dx, 0x03FD      ; LSR
  in al, dx
  test al, 0x20       ; Transmit holding register empty?
  jz .wait
  mov dx, 0x03F8
  mov al, ah
  out dx, al          ; Send byte
  ret

; Send null-terminated string over COM1
; rsi: string pointer
serial_puts:
  push rax
.loop:
  mov al, [rsi]
  test al, al
  jz .done
  call serial_putc
  inc rsi
  jmp .loop
.done:
  pop rax
  ret

; Send string + CRLF over COM1
; rsi: string pointer
serial_putln:
  call serial_puts
  mov al, 0x0D        ; CR
  call serial_putc
  mov al, 0x0A        ; LF
  call serial_putc
  ret

section .rodata
msg_lm_segs:         db "[BOOT(64)] segments zeroed", 0
msg_lm_call_kernel: db "[BOOT(64)] calling kernel_main", 0
msg_hi_started:     db "[BOOT(64)] high half started", 0
