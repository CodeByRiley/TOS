; kernel/arch/x86_64/boot/header.asm , Multiboot2 header.
;
; Magic, checksum and multiboot2 header.

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
	dd 0                        		  ; width, 0 is no preference so the bootloader will choose
	dd 0                          	  ; height
	dd 0                              ; bpp
fb_tag_end:

	; end tag
	align 8
	dw 0
	dw 0
	dd 8
header_end:
