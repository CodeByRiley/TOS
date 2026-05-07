global _start
extern main
extern exit

section .text
_start:
	xor rbp, rbp 		; clear frame ptr

	; argc, argv ( rsp[0] = argc, rsp[8] = argv[8])
	mov rdi, [rsp]
	mov rsi, [rsp + 8]

	call main
	mov rdi, rax 		; main return
	call exit
.hang:
	hlt
	jmp .hang
