/*
 * Linker script for SN110 bFLT binary
 *
 * Lays out sections for conversion to bFLT v2 format:
 *   - Text at address 0 (will be relocated by bFLT loader)
 *   - Data immediately after text
 *   - BSS after data
 *
 * The bFLT converter reads section addresses to build the header.
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
    /* Code starts at address 0 — bFLT loader will relocate */
    . = 0x0;

    .text : {
        *(.text .text.*)
        *(.gnu.warning)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    /* Align data to 4-byte boundary */
    . = ALIGN(4);
    _etext = .;

    .data : {
        _data_start = .;
        *(.data .data.*)
        *(.got .got.plt)
    }

    . = ALIGN(4);
    _edata = .;

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(4);
    _end = .;

    /* Discard unneeded sections */
    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.eh_frame*)
    }
}
