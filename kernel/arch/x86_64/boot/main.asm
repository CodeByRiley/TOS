; kernel/arch/x86_64/boot/main.asm — 32-bit boot stub.
;
; Convinces a CPU currently pretending it's 1986 to act like it's at
; least 2003. Checks valid Multiboot2 magic, cpuid, long mode, and SSE —
; every capability we already know is there but refuse to assume because
; someone, somewhere, is booting this on something weird. Then builds a
; 1 GiB identity-mapped pagetable with 2 MiB huge pages (one PML4 entry
; -> one PDPT -> one PD with 512 entries), flips CR0.PG, far-jumps to
; 64-bit. Total runtime: roughly one blink.

global start
extern long_mode_start
section .boot.text progbits alloc exec nowrite align=16
bits 32

mb2_magic equ 0x36D76289
; 0x36D76289
; 0xE85250D6

; entry point from GRUB
; eax: multiboot2 magic
; ebx: multiboot2 info pointer
start:
	mov esp, stack_top         ; set up stack
	mov [mb2_save], ebx        ; stash mb2 ptr (survives clear/cpuid clobbers)

	call check_multiboot       ; verify eax magic before clear destroys it
	call serial_init           ; bring up COM1 for boot logging
	call clear                 ; blank VGA buffer

	mov esi, msg_boot
	call serial_putln

	mov esi, msg_chk_cpuid
	call serial_putln
	call check_cpuid

	mov esi, msg_chk_sse
	call serial_putln
	call check_sse

	mov esi, msg_chk_lm
	call serial_putln
	call check_long_mode

	mov esi, msg_setup_pt
	call serial_putln
	call setup_page_tables

	mov esi, msg_enable_paging
	call serial_putln
	call enable_paging

	mov esi, msg_enable_sse
	call serial_putln
	call enable_sse

	mov esi, msg_jump_lm
	call serial_putln

	mov edi, [mb2_save]        ; restore mb2 ptr; rdi = arg0 for kernel_main
	lgdt [gdt64.pointer]       ; load 64-bit GDT
	jmp gdt64.code_segment:long_mode_start  ; far jump into 64-bit code
	hlt                        ; unreachable

; verify multiboot2 magic in eax
; eax: magic value from GRUB
check_multiboot:
	cmp eax, mb2_magic        ; multiboot2 bootloader magic
	jne .no_multiboot
	ret
.no_multiboot:
	mov esi, msg_no_multiboot
	jmp error

; verify cpuid is supported by toggling EFLAGS bit 21 (ID)
check_cpuid:
	pushfd                     ; copy EFLAGS to stack
	pop eax
	mov ecx, eax               ; save original
	xor eax, 1 << 21           ; flip ID bit
	push eax
	popfd                      ; load modified EFLAGS
	pushfd                     ; read back
	pop eax
	push ecx
	popfd                      ; restore original EFLAGS
	cmp eax, ecx               ; flipped bit stuck = no cpuid
	je .no_cpuid
	ret
.no_cpuid:
	mov esi, msg_no_cpuid
	jmp error

; verify SSE supported via cpuid leaf 1, edx bit 25
check_sse:
	mov eax, 0x1
	cpuid
	test edx, 1 << 25          ; SSE flag
	jz .no_sse
	ret
.no_sse:
	mov esi, msg_no_sse
	jmp error

; verify long mode (edx bit 29) and NX (edx bit 20) via extended cpuid
; leaf 0x80000001. Both bits come from one cpuid; enable_paging sets
; EFER.LME and EFER.NXE off the back of this.
check_long_mode:
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001        ; extended cpuid available?
	jb .no_long_mode
	mov eax, 0x80000001
	cpuid
	test edx, 1 << 29          ; long mode flag
	jz .no_long_mode
	test edx, 1 << 20          ; NX flag
	jz .no_nx
	ret
.no_long_mode:
	mov esi, msg_no_long_mode
	jmp error
.no_nx:
	mov esi, msg_no_nx
	jmp error

HHDM_GIB equ 4

; Build low identity and kernel aliases plus a 4 GiB bootstrap HHDM.
setup_page_tables:
  ; PML4[0] and PML4[511] share this PDPT.
  ; It provides the low identity map and the kernel alias.
  mov eax, page_table_l3
  or eax, 0b11
  mov [page_table_l4], eax              ; PML4[0]: low identity map
  mov [page_table_l4 + 511 * 8], eax    ; PML4[511]: kernel high half

  ; PML4[256] owns the separate HHDM range:
  ; 0xffff800000000000 + physical address.
  mov eax, page_table_hhdm_l3
  or eax, 0b11
  mov [page_table_l4 + 256 * 8], eax    ; PML4[256]: HHDM

  ; The shared low/kernel PDPT:
  mov eax, page_table_l2
  or eax, 0b11
  mov [page_table_l3], eax              ; PDPT[0]: low 0..1 GiB
  mov [page_table_l3 + 510 * 8], eax    ; PDPT[510]: kernel alias

  ; Fill its page directory: 512 * 2 MiB = 1 GiB.
  mov eax, 0x83                          ; present | writable | PS
  xor ecx, ecx
.low_pd_loop:
  mov [page_table_l2 + ecx * 8], eax
  mov dword [page_table_l2 + ecx * 8 + 4], 0
  add eax, 0x200000
  inc ecx
  cmp ecx, 512
  jne .low_pd_loop

  ; HHDM PDPT[0..3] -> four consecutive page-directory pages.
  mov edi, page_table_hhdm_l3
  mov eax, page_table_hhdm_l2
  mov ecx, HHDM_GIB
.hhdm_pdpt_loop:
    mov ebx, eax
    or ebx, 0b11
    mov [edi], ebx
    mov dword [edi + 4], 0
    add edi, 8
    add eax, 4096
    dec ecx
    jnz .hhdm_pdpt_loop

    ; Fill all four HHDM page directories.
    ; eax: physical address bits 0..31
    ; edx: physical address bits 32..63
    mov edi, page_table_hhdm_l2
    xor eax, eax
    xor edx, edx
    mov ebp, HHDM_GIB
.hhdm_pd_loop:
    mov ecx, 512
.hhdm_entry_loop:
		; eax = phys base low
		; edx = phys base high
    mov ebx, eax
    or ebx, 0x83                          ; present | writable | PS
    mov [edi], ebx
    ; The HHDM is a data-only window over physical memory: kernel code runs
    ; from the low identity map and the PML4[511] alias, never from here.
    ; edx is the entry's high dword, so bit 31 here is entry bit 63 (NX).
    ; Requires EFER.NXE, which enable_paging sets before CR0.PG — and
    ; check_long_mode has already proven the CPU supports it.
    or edx, (1 << 31)                     ; NX
    mov [edi + 4], edx

    ; edx carries the physical high dword and the NX bit together, so the
    ; adc below increments a value with bit 31 already set. That is fine:
    ; the carry lands in the address bits and the `or` above re-sets NX
    ; every iteration. At HHDM_GIB = 4 the carry never fires at all.
    add eax, 0x200000                     ; next 2 MiB physical range
    adc edx, 0                             ; increment after 4 GiB
    add edi, 8
    dec ecx
    jnz .hhdm_entry_loop

    dec ebp
    jnz .hhdm_pd_loop

    ret

; load page tables, enable PAE, long mode, paging
enable_paging:
	mov eax, page_table_l4
	mov cr3, eax               ; cr3 = PML4 base

	mov eax, cr4
	or eax, 1 << 5             ; CR4.PAE
	mov cr4, eax

	mov ecx, 0xC0000080        ; IA32_EFER
	rdmsr
	or eax, 1 << 8             ; EFER.LME (long mode enable)
	or eax, 1 << 11            ; EFER.NXE (no-execute enable)
	wrmsr

	mov eax, cr0
	or eax, 1 << 31            ; CR0.PG (paging enable)
	mov cr0, eax
	ret

; enable SSE so C compiler can emit SSE code
enable_sse:
	mov eax, cr0
	and ax, 0xFFFB             ; clear CR0.EM (no FPU emulation)
	or  ax, 0x2                ; set CR0.MP (monitor coprocessor)
	mov cr0, eax

	mov eax, cr4
	or  ax, 3 << 9             ; set CR4.OSFXSR | CR4.OSXMMEXCPT
	mov cr4, eax
	ret

; log to serial AND VGA, then halt
; esi: message
error:
	push esi                   ; save caller's message
	mov esi, msg_error_prefix
	call serial_puts           ; "ERROR: " (no newline)
	pop esi
	push esi
	call serial_putln          ; specific message + CRLF

	mov edi, 0xB8000           ; VGA cursor at top-left
	mov esi, msg_error_prefix
	call print                 ; VGA "ERROR: "
	pop esi
	call print                 ; VGA specific message
	hlt

; VGA print
; esi: null-terminated string
; edi: vmem cursor (advanced past last char written)
print:
.loop:
	mov al, [esi]              ; load next char
	cmp al, 0
	je .done                   ; null terminator -> stop
	mov [edi], al              ; write char
	mov byte [edi+1], 0x0F     ; attribute = white on black
	add edi, 2                 ; advance cursor (char + attribute)
	inc esi
	jmp .loop
.done:
	ret

; blank VGA text buffer with spaces
clear:
	mov edi, 0xb8000           ; VGA buffer base
	mov ecx, 80 * 25           ; total cells
	mov eax, 0x0f20            ; attribute 0x0F (white/black) + space char
	rep stosw                  ; fill word-by-word
	ret

; initialise COM1 @ 0x3F8, 38400 baud 8N1
serial_init:
	mov dx, 0x3F9              ; IER
	xor al, al
	out dx, al                 ; disable interrupts

	mov dx, 0x3FB              ; LCR
	mov al, 0x80
	out dx, al                 ; DLAB on (access divisor)

	mov dx, 0x3F8              ; DLL
	mov al, 0x03
	out dx, al                 ; divisor low = 3 (115200/3 = 38400)

	mov dx, 0x3F9              ; DLH
	xor al, al
	out dx, al                 ; divisor high = 0

	mov dx, 0x3FB              ; LCR
	mov al, 0x03
	out dx, al                 ; 8 data bits, no parity, 1 stop, DLAB off

	mov dx, 0x3FA              ; FCR
	mov al, 0xC7
	out dx, al                 ; enable + clear FIFO, 14-byte trigger

	mov dx, 0x3FC              ; MCR
	mov al, 0x0B
	out dx, al                 ; DTR + RTS + OUT2 (IRQ enable line)
	ret

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
; esi: string pointer
serial_puts:
	push eax
.loop:
	mov al, [esi]
	cmp al, 0
	je .done
	call serial_putc
	inc esi
	jmp .loop
.done:
	pop eax
	ret

; send string + CRLF over COM1
; esi: string pointer
serial_putln:
	call serial_puts
	mov al, 0x0D               ; CR
	call serial_putc
	mov al, 0x0A               ; LF
	call serial_putc
	ret

section .boot.bss nobits alloc noexec write align=4096
align 4096
page_table_l4:
	resb 4096
page_table_l3:
	resb 4096
page_table_l2:
	resb 4096

page_table_hhdm_l3:
  resb 4096                    ; HHDM PDPT
page_table_hhdm_l2:
  resb 4096 * HHDM_GIB          ; one PD per bootstrap HHDM GiB

stack_bottom:
	resb 4096 * 4
stack_top:
mb2_save:
	resd 1                     ; saved multiboot2 info pointer

section .boot.rodata progbits alloc noexec nowrite align=8
msg_error_prefix:  db "[ERROR] ", 0
msg_no_multiboot:  db "NO MULTIBOOT", 0
msg_no_nx:         db "NO NX", 0
msg_no_cpuid:      db "NO CPUID", 0
msg_no_sse:        db "NO SSE", 0
msg_no_long_mode:  db "NO LONG MODE", 0

msg_boot:          db "[BOOT(32)] entered 32-bit start", 0
msg_chk_cpuid:     db "[BOOT(32)] checking cpuid", 0
msg_chk_sse:       db "[BOOT(32)] checking sse", 0
msg_chk_lm:        db "[BOOT(32)] checking long mode", 0
msg_setup_pt:      db "[BOOT(32)] setting up page tables", 0
msg_enable_paging: db "[BOOT(32)] enabling paging", 0
msg_enable_sse:    db "[BOOT(32)] enabling sse", 0
msg_jump_lm:       db "[BOOT(32)] jumping to long mode", 0

; 64-bit GDT: null + flat code segment
gdt64:
	dq 0                       ; null entry (required)
.code_segment: equ $ - gdt64
	dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)  ; exec, code, present, 64-bit
.pointer:
	dw $ - gdt64 - 1           ; limit = size - 1
	dq gdt64                   ; base
