/*
 * boot.ld: the gemifi bootloader on an STM32F1 (bootloader/stm32f1), in the
 * BOARD_BOOT_SIZE bytes below the app (9 KB on the Code 61: 0x08000000-
 * 0x080023FF). Apps keep their own layout (ld/app.ld: image at
 * BOARD_APP_BASE, .gfw header at +0x130). board_boot.ld (generated from the
 * board profile) holds the MEMORY regions and the BOARD_* symbols.
 *
 *   BOARD_FLASH_BASE          vectors (16 core + 60 IRQ = 0x130 bytes)
 *   BOARD_FLASH_BASE + 0x130  boot marker, 16 bytes (gemifi_image.h: 'GMBL')
 *   BOARD_FLASH_BASE + 0x140  code
 *
 * As in app.ld, the initial SP stays below the top of RAM.
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
        _vectors_end = .;
        . = 0x130;
        KEEP(*(.gemifi_boot_mark))
        _boot_mark_end = .;
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

    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        *(.noinit .noinit.*)
        . = ALIGN(4);
        _enoinit = .;
    } > RAM

    _estack = ORIGIN(RAM) + LENGTH(RAM) - BOARD_STACK_GUARD;
    _image_end = LOADADDR(.data) + SIZEOF(.data);

    ASSERT(ADDR(.text) == BOARD_FLASH_BASE, "bootloader must start at BOARD_FLASH_BASE")
    ASSERT(_vectors_end <= BOARD_FLASH_BASE + 0x130, "vector table overlaps the boot marker")
    ASSERT(_boot_mark_end == BOARD_FLASH_BASE + 0x140, "boot marker missing or not 16 bytes")
    ASSERT(_image_end <= BOARD_APP_BASE, "bootloader runs into the app at BOARD_APP_BASE")
    ASSERT(_enoinit + 1024 <= _estack, "less than 1 KB of RAM left for the stack")
    ASSERT(_estack <= BOARD_MAILBOX, "the stack top runs into the SRAM mailbox (sys.h)")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
