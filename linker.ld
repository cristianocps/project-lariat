ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4K)
    {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
    }

    .data : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
        *(.ldata)
        *(.ldata.*)
    }

    /* -mcmodel=large puts large/mergeable BSS (e.g. the 256 KiB block cache) in
     * .lbss; capturing it here is what keeps __bss_end past every BSS object so
     * the PMM bitmap (placed at __bss_end) never overlaps a live BSS array. */
    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        *(.lbss)
        *(.lbss.*)
        __bss_end = .;
    }
}
