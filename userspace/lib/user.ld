ENTRY(_start)
SECTIONS
{
    . = 0x0000000040000000;          /* 1 GiB — outside boot identity map */
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : ALIGN(0x1000) { *(.data .data.*) }
    .bss  : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) }
}
