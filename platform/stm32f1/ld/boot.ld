/*
 * boot.ld: the gemifi bootloader on an STM32F1 (bootloader/stm32f1), in the
 * 9 KB below the app: 0x08000000-0x080023FF. Apps keep their own layout
 * (ld/app.ld: image at 0x08002400, .gfw header at +0x130).
 *
 *   0x08000000  vectors (16 core + 60 IRQ = 0x130 bytes)
 *   0x08000130  boot marker, 16 bytes (gemifi_image.h: 'GMBL')
 *   0x08000140  code
 *
 * RAM: 20 KB. As in app.ld, the initial SP stays below the top of RAM.
 *
 * SPDX-License-Identifier: MIT
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 0x2400
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

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

    STACK_GUARD = 0x100;
    _estack = ORIGIN(RAM) + LENGTH(RAM) - STACK_GUARD;
    _image_end = LOADADDR(.data) + SIZEOF(.data);

    ASSERT(ADDR(.text) == 0x08000000, "bootloader must start at 0x08000000")
    ASSERT(_vectors_end <= 0x08000130, "vector table overlaps the boot marker")
    ASSERT(_boot_mark_end == 0x08000140, "boot marker missing or not 16 bytes")
    ASSERT(_image_end <= 0x08002400, "bootloader runs into the app at 0x08002400")
    ASSERT(_enoinit + 1024 <= _estack, "less than 1 KB of RAM left for the stack")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
