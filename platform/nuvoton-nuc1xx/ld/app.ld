/*
 * app.ld: gemifi application in NUC1xx APROM (32 KB at 0x0).
 *
 * Layout: vector table 0x000-0x0BF (16 core + 32 IRQ entries), the 32-byte
 * .gfw header at 0x0C0 (protocol/sysex.md 5), then code.
 * RAM ends at the 16-byte mailbox at 0x2000_0FF0 (sysex.md 6).
 *
 * SPDX-License-Identifier: MIT
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 32K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4K - 16
}

ENTRY(Reset_Handler)

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        _vectors_end = .;
        . = 0x0C0;
        KEEP(*(.gemifi_hdr))
        _gemifi_hdr_end = .;
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    _estack = ORIGIN(RAM) + LENGTH(RAM);
    _image_end = LOADADDR(.data) + SIZEOF(.data);

    ASSERT(_vectors_end <= 0x0C0, "vector table overlaps the image header")
    ASSERT(_gemifi_hdr_end == 0x0E0, "image header missing or not 32 bytes")
    ASSERT(_ebss + 512 <= _estack, "less than 512 B of RAM left for the stack")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
