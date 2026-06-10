; src/impl/x86_64/isr.asm — IDT entry stubs + common dispatcher.
;
; 48 vectors (0..31 = CPU exceptions, 32..47 = PIC-remapped IRQs), each
; entered via its own tiny stub so we know which vector fired and so
; error-code-pushing exceptions look the same as non-pushing ones to the
; C handler. The dummy-zero push for no-err vectors exists because x86
; couldn't agree with itself in 1985 about whether the CPU should push
; an error code, and we are still apologising for that decision.

extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0          ; dummy error code
    push %1         ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push %1         ; CPU already pushed err code
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; Save every general-purpose register. Interrupts are asynchronous — there is
; no calling convention to lean on, no caller-saved/callee-saved distinction
; we get to honour. Skipping any of these is a bug, full stop.
isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        ; SysV ABI: first arg = rdi = pointer to regs
    cld                 ; required by SysV before calling C
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16         ; discard vector + err code
    iretq

; expose stub addresses as a C-visible table
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr %+ i
%assign i i+1
%endrep
