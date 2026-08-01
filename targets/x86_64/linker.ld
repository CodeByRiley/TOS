ENTRY(start)

KERNEL_PHYS_BASE = 1M;
KERNEL_VMA       = 0xffffffff80000000;

/* ELF p_flags: PF_R=4, PF_W=2, PF_X=1. */
PHDRS
{
    boot_text PT_LOAD FLAGS(5); /* R-X */
    boot_data PT_LOAD FLAGS(6); /* RW- */
    text      PT_LOAD FLAGS(5); /* R-X */
    rodata    PT_LOAD FLAGS(4); /* R-- */
    data      PT_LOAD FLAGS(6); /* RW- */
}

SECTIONS
{
    . = KERNEL_PHYS_BASE;
    _kernel_phys_start = .;

    /* Keep the Multiboot header near the start of the image. Boot strings
     * share the executable segment but remain non-writable. */
    .boot.text : ALIGN(4096)
    {
        KEEP(*(.multiboot_header))
        *(.boot.text*)
        *(.boot.rodata*)
        . = ALIGN(4096);
    } :boot_text

    /* Early page tables and the temporary stack are writable but never
     * executable. NOLOAD turns resb storage into segment BSS. */
    .boot.bss (NOLOAD) : ALIGN(4096)
    {
        *(.boot.bss*)
        . = ALIGN(4096);
    } :boot_data

    . += KERNEL_VMA;
    _kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096)
    {
        *(.text*)
        . = ALIGN(4096);
    } :text

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4096)
    {
        *(.rodata*)
        *(.eh_frame*)
        *(.gcc_except_table*)
        . = ALIGN(4096);
    } :rodata

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4096)
    {
        *(.data*)
        . = ALIGN(4096);
    } :data

    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4096)
    {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4096);
    } :data

    _kernel_end = .;
    _kernel_phys_end = LOADADDR(.bss) + SIZEOF(.bss);

    /DISCARD/ :
    {
        *(.comment)
        *(.note.GNU-stack)
    }
}
