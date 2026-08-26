/* userspace/lib/crt0_pe.S , process entry for PE builds.
 *
 * The PE counterpart of lib/crt0.s, which is NASM/Intel syntax and emits
 * ELF; this is gas/AT&T so mingw can assemble it into a PE object.
 *
 * The kernel hands both formats the same stack, built by process.c:
 *
 *     [rsp]      argc
 *     [rsp+8]    argv[0]
 *     ...        argv[argc-1], then a NULL
 *
 * and guarantees rsp is 16-byte aligned on entry.
 *
 * The difference is where main wants those two values. crt0.s puts them
 * in rdi/rsi for the SysV convention; mingw's main reads rcx/rdx. It also
 * requires the caller to reserve 32 bytes of shadow space, and expects
 * rsp to be 16-byte aligned at the call site , which `sub $32` preserves,
 * so main sees the misalign-by-8 that a pushed return address is supposed
 * to produce.
 */
	.text
	.globl _start
_start:
	xorq	%rbp, %rbp          /* end of the frame chain           */

	movq	(%rsp), %rcx        /* argc  -> MS ABI first argument   */
	leaq	8(%rsp), %rdx       /* argv  -> MS ABI second argument  */

	subq	$32, %rsp           /* shadow space; keeps rsp % 16 == 0 */
	call	main

	movq	%rax, %rcx          /* main's return -> exit's argument */
	call	exit

	/* exit does not return. If it ever did, stop here rather than
	 * falling into whatever follows in .text. */
1:	hlt
	jmp	1b

/* gcc emits `call __main` as the first instruction of any function named
 * main , the hook that would normally run C++ global constructors and the
 * rest of mingw's CRT startup. libmingw32 supplies it; -nostdlib does not.
 * There is nothing to initialise here, so it returns immediately. */
	.globl	__main
__main:
	ret
