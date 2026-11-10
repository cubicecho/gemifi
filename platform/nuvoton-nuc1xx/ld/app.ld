/*
 * app.ld: gemifi application in NUC1xx APROM (at 0x0; 32 KB on the
 * NUC120LC1BN).
 *
 * Layout: vector table 0x000-0x0BF (16 core + 32 IRQ entries), the 32-byte
 * .gfw header at 0x0C0 (protocol/sysex.md 5), then code.
 * RAM ends at the 16-byte mailbox in the top of SRAM (sysex.md 6).
 * board_app.ld (generated from the board profile, found through -L) holds
 * the MEMORY regions: APROM, and SRAM up to the mailbox.
 *
 * SPDX-License-Identifier: MIT
 */
INCLUDE board_app.ld

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
