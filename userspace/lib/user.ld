ENTRY(_start)

PHDRS
{
    rodata PT_LOAD FLAGS(4); /* R-- : .rodata + .appinfo */
    text   PT_LOAD FLAGS(5); /* R-X */
    data   PT_LOAD FLAGS(6); /* RW- */
}

SECTIONS
{
    . = 0x40000000;

    .appinfo ALIGN(4) :
    {
        __appinfo_start = .;
        KEEP(*(.appinfo))
        __appinfo_end = .;
    } :rodata

    .rodata ALIGN(16) :
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);

    .text :
    {
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .data :
    {
        *(.data .data.*)
    } :data

    .init_array ALIGN(8) :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data

    .fini_array ALIGN(8) :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data

    .bss ALIGN(0x1000) :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data
}
