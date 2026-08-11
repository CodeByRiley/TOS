; kernel/arch/x86_64/boot/ap_trampoline.asm — AP startup trampoline.
;
; Copied verbatim to physical address 0x8000 by the BSP before sending
; SIPI. The AP wakes in 16-bit real mode at CS=0x0800, IP=0x0000 (linear
; 0x8000) and trampolines itself up to 64-bit long mode, then jumps to a
; C entry point whose pointer is patched in at well-known offsets near
; the end of this blob.
;
; Page tables: we use the *existing* kernel PML4 (its physical address is
; patched in below) because boot identity-maps the low 1 GiB, which covers
; this trampoline itself, and also has every kernel virtual mapping we'll
; need once we land in C.
;
; Patch layout (offsets from 0x8000):
;   ap_pml4_phys  : dword — kernel PML4 physical address (32-bit fits, we're
;                            below 4 GiB at this point in boot)
;   ap_stack_top  : qword — top of this AP's kernel stack
;   ap_c_entry    : qword — virtual address of ap_main()
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

    mov eax, cr4
    or  eax, 1 << 5             ; CR4.PAE
    mov cr4, eax

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
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [ap_stack_top]
    mov rax, [ap_c_entry]
    ; Pass our cpu_id via rdi. BSP patched it at ap_cpu_id.
    mov edi, [ap_cpu_id]
    jmp rax

; ---------------------------------------------------------------- GDTs

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
    dq 0x00AF92000000FFFF       ; 64-bit data, ring 0, write
ap_gdt64_end:
ap_gdt64_ptr:
    dw ap_gdt64_end - ap_gdt64 - 1
    dq ap_gdt64

; ---------------------------------------------------------------- patch slots
align 8
ap_pml4_phys:   dd 0
ap_cpu_id:      dd 0
ap_stack_top:   dq 0
ap_c_entry:     dq 0

; Export the patch-slot offsets so C can poke them via well-known constants.
; nasm doesn't expose these names to ld in -f bin, so we publish them via
; equate comments — the C side hardcodes the same offsets, computed from
; this file's layout. See `smp.c` and `AP_TRAMPOLINE_*_OFF`.

ap_trampoline_end:
