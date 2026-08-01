global _start
extern main
extern exit

section .text
_start:
	xor rbp, rbp 		; clear frame ptr

	; Initial stack: [rsp] = argc, [rsp+8...] = argv pointers + NULL.
	mov rdi, [rsp]
	lea rsi, [rsp + 8]

	call main
	mov rdi, rax 		; main return
	call exit
.hang:
	hlt
	jmp .hang
