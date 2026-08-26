ENTRY(_start)
SECTIONS
{
    . = 0x0000000040000000;          /* 1 GiB , outside boot identity map */
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : ALIGN(0x1000) { *(.data .data.*) }
    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(.init_array .init_array.* .ctors .ctors.*))
        PROVIDE_HIDDEN(__init_array_end = .);
    }
    .fini_array : ALIGN(8) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(.fini_array .fini_array.* .dtors .dtors.*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }
    .bss  : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) }
}
