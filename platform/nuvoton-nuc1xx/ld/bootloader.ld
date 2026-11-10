/*
 * bootloader.ld: gemifi bootloader for NUC1xx LDROM.
 *
 * Linked at 0x0 because LDROM is mapped at 0x0 while it runs (CONFIG0
 * CBS = 0, bootloader-design.md 1). Programmed at 0x0010_0000 over SWD.
 * The TEST_IN_APROM build uses this same script and is programmed at 0x0.
 *
 * RAM ends 16 bytes early: the top 16 bytes (0x2000_0FF0..0x2000_0FFF on
 * 4 KB) are the no-init mailbox shared with the app (protocol/sysex.md 6).
 * The initial stack pointer is the mailbox address. board_boot.ld
 * (generated from the board profile) holds the MEMORY regions.
 *
 * SPDX-License-Identifier: MIT
 */
INCLUDE board_boot.ld

ENTRY(Reset_Handler)

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
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

    ASSERT(_image_end <= 4096, "bootloader does not fit in 4 KB LDROM")
    ASSERT(SIZEOF(.data) == 0, "bootloader has .data, but startup.c is built with STARTUP_NO_DATA")
    ASSERT(_ebss + 512 <= _estack, "less than 512 B of RAM left for the stack")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
