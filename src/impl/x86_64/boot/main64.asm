; src/impl/x86_64/boot/main64.asm — 64-bit landing pad.
;
; Reached from main.asm via a far jump through the 64-bit GDT. Zeros
; segment registers (long mode treats them as decorative, but the CPU
; still faults if they're wrong), prints a few breadcrumbs over serial
; so post-mortems have something to read, then jumps to kernel_main.
;
; serial_putc/puts/putln are copy-pasted from main.asm with 32-bit regs
; swapped for 64-bit. Yes that's duplication. No it's not getting
; deduped — the 32-bit copy literally cannot link against the 64-bit
; one. Six lines of `mov` vs. a weekend with a linker script.

global long_mode_start
extern kernel_main
section .text
bits 64

; entry point from 32-bit boot via far jump through 64-bit GDT
; rdi: mb2 info pointer (passed through to kernel_main as arg0)
long_mode_start:
	push rdi                   ; preserve mb2 ptr through logging calls

	mov rsi, msg_lm_entered
	call serial_putln

	mov ax, 0                  ; zero out segment registers (unused in long mode)
	mov ss, ax
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax

	mov rsi, msg_lm_segs
	call serial_putln

	mov rsi, msg_lm_call_kernel
	call serial_putln

	pop rdi                    ; restore mb2 ptr for kernel_main(arg0)
	call kernel_main
	hlt                        ; unreachable if kernel_main returns

; COM1 already initialised in 32-bit boot, helpers below reuse same hardware

; send one byte over COM1
; al: char to send
; clobbers ah, dx
serial_putc:
	mov ah, al                 ; stash char (in serves into al)
.wait:
	mov dx, 0x3FD              ; LSR
	in al, dx
	test al, 0x20              ; transmit holding empty?
	jz .wait
	mov dx, 0x3F8
	mov al, ah
	out dx, al                 ; send byte
	ret

; send null-terminated string over COM1
; rsi: string pointer
serial_puts:
	push rax
.loop:
	mov al, [rsi]              ; load next char
	test al, al
	jz .done                   ; null terminator -> stop
	call serial_putc
	inc rsi
	jmp .loop
.done:
	pop rax
	ret

; send string + CRLF over COM1
; rsi: string pointer
serial_putln:
	call serial_puts
	mov al, 0x0D               ; CR
	call serial_putc
	mov al, 0x0A               ; LF
	call serial_putc
	ret

section .rodata
msg_lm_entered:     db "[boot (64)] long mode entered", 0
msg_lm_segs:        db "[boot (64)] segments zeroed", 0
msg_lm_call_kernel: db "[boot (64)] calling kernel_main", 0
