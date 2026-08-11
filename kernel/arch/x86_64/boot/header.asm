; kernel/arch/x86_64/boot/header.asm — Multiboot2 header.
;
; First thing GRUB looks at when it decides whether the file you just
; handed it is a kernel or a sandwich. Magic + checksum + a single
; optional tag asking GRUB to give us a 1920x1080x32bpp framebuffer.
; The framebuffer "request" is advisory — GRUB will hand back something
; else if it doesn't like the size — but every sane firmware honours it.
; Insane firmware can write in.

section .multiboot_header progbits alloc noexec nowrite align=8
align 8
header_start:
	dd 0xe85250d6                     ; magic
	dd 0                              ; arch (i386 protected mode)
	dd header_end - header_start      ; header length
	dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

	; framebuffer tag: ask GRUB to set graphics mode
	align 8
fb_tag_start:
	dw 5                              ; type = framebuffer request
	dw 0                              ; flags (0 = required)
	dd fb_tag_end - fb_tag_start      ; size
	dd 1920                        		; width (0 = bootloader chooses)
	dd 1080                          	; height
	dd 32                             ; bpp
fb_tag_end:

	; end tag
	align 8
	dw 0
	dw 0
	dd 8
header_end:
