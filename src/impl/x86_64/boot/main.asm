; 32-bit boot stub. Job description: convince a CPU that's currently
; pretending it's 1986 to act like it's at least 2003. We check that the
; bootloader handed us valid magic, that cpuid exists, that long mode exists,
; that SSE exists — basically every capability we already know is there but
; refuse to assume because someone, somewhere, is booting this on something
; weird. Then build a 1 GiB identity-mapped pagetable with 2 MiB huge pages
; (one PML4 entry pointing at one PDPT pointing at one PD with 512 entries),
; flip CR0.PG, far-jump to 64-bit. Total runtime: roughly one blink.

global start
extern long_mode_start
section .text
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

; verify long mode supported via extended cpuid leaf 0x80000001, edx bit 29
check_long_mode:
	mov eax, 0x80000000
	cpuid
	cmp eax, 0x80000001        ; extended cpuid available?
	jb .no_long_mode
	mov eax, 0x80000001
	cpuid
	test edx, 1 << 29          ; long mode flag
	jz .no_long_mode
	ret
.no_long_mode:
	mov esi, msg_no_long_mode
	jmp error

; build identity-mapping page tables for first 1 GiB using 2 MiB huge pages
setup_page_tables:
	mov eax, page_table_l3
	or eax, 0b11               ; present, writable
	mov [page_table_l4], eax   ; PML4[0] -> PDPT

	mov eax, page_table_l2
	or eax, 0b11               ; present, writable
	mov [page_table_l3], eax   ; PDPT[0] -> PD

	; Walk PD entries with a running running-add instead of a mul per iter.
	; 512 iterations isn't a perf cliff, but mul-in-a-tight-loop is the kind
	; of thing you grep for during a "why does boot take 200ms" investigation
	; six months from now. eax = next PTE value, advances by 2 MiB each step.
	mov eax, 0b10000011        ; PTE flags: present + writable + PS (huge)
	xor ecx, ecx               ; index 0..511
.loop:
	mov [page_table_l2 + ecx * 8], eax
	add eax, 0x200000          ; advance physical addr by 2 MiB
	inc ecx
	cmp ecx, 512               ; full PD = 512 entries = 1 GiB
	jne .loop
	ret

; load page tables, enable PAE, long mode, paging
enable_paging:
	mov eax, page_table_l4
	mov cr3, eax               ; cr3 = PML4 base

	mov eax, cr4
	or eax, 1 << 5             ; CR4.PAE
	mov cr4, eax

	mov ecx, 0xC0000080        ; EFER MSR
	rdmsr
	or eax, 1 << 8             ; EFER.LME (long mode enable)
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

section .bss
align 4096
page_table_l4:
	resb 4096
page_table_l3:
	resb 4096
page_table_l2:
	resb 4096
stack_bottom:
	resb 4096 * 4
stack_top:
mb2_save:
	resd 1                     ; saved multiboot2 info pointer

section .rodata
msg_error_prefix:  db "ERROR: ", 0
msg_no_multiboot:  db "NO MULTIBOOT", 0
msg_no_cpuid:      db "NO CPUID", 0
msg_no_sse:        db "NO SSE", 0
msg_no_long_mode:  db "NO LONG MODE", 0

msg_boot:          db "[boot (32)] entered 32-bit start", 0
msg_chk_cpuid:     db "[boot (32)] checking cpuid", 0
msg_chk_sse:       db "[boot (32)] checking sse", 0
msg_chk_lm:        db "[boot (32)] checking long mode", 0
msg_setup_pt:      db "[boot (32)] setting up page tables", 0
msg_enable_paging: db "[boot (32)] enabling paging", 0
msg_enable_sse:    db "[boot (32)] enabling sse", 0
msg_jump_lm:       db "[boot (32)] jumping to long mode", 0

; 64-bit GDT: null + flat code segment
gdt64:
	dq 0                       ; null entry (required)
.code_segment: equ $ - gdt64
	dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)  ; exec, code, present, 64-bit
.pointer:
	dw $ - gdt64 - 1           ; limit = size - 1
	dq gdt64                   ; base
