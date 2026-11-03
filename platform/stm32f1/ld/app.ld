/*
 * app.ld: gemifi application on an STM32F1 behind the vendor bootloader
 * (M-Audio Code 61; research/devices/m-audio-code61/analysis/
 * custom-firmware-path.md 4).
 *
 * Flash map:
 *   0x08000000-0x080023FF  vendor bootloader: never touched
 *   0x08002400-0x0801AFFF  this image: vectors 0x000-0x12F (16 + 60
 *                          entries), .gfw header at +0x130 (0x08002530,
 *                          protocol/sysex.md 5), then code
 *   0x0801B000-0x0801B3FF  vendor "app valid" page, whole-image checksum at
 *                          0x0801B3FE: written by the host packer, not here
 *   0x0801B400-0x0801FBFF  vendor settings "EEPROM" (gemifi profile pages
 *                          BOARD_CONFIG_BASE/PAGES live in here)
 *   0x0801FC00-0x0801FFFF  vendor update-request flag page (recovery.h)
 *
 * RAM: 20 KB (STM32F103RBT6, confirmed on hardware 2026-11-03).
 *
 * The initial SP must NOT be the top of RAM. The vendor bootloader loads
 * MSP from our vector table and then accesses memory at the new SP itself
 * (seen over SWD: precise bus fault, BFAR = 0x20005000, in the bootloader
 * at 0x08000770, before our Reset_Handler ran). With SP = 0x20005000 that
 * is past the end of RAM, so the bootloader hard-faults and the board looks
 * dead. The stock app's SP is 0x200041A0. Keep STACK_GUARD bytes free above
 * the initial SP for whatever the bootloader touches there.
 *
 * SPDX-License-Identifier: MIT
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08002400, LENGTH = 0x0801B000 - 0x08002400
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

    /* Not zeroed at reset: survives a soft/watchdog reset (probe app). */
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

    ASSERT(ADDR(.text) == 0x08002400, "image must start at 0x08002400")
    ASSERT(_vectors_end <= 0x08002530, "vector table overlaps the image header")
    ASSERT(_gemifi_hdr_end == 0x08002550, "image header missing or not 32 bytes")
    ASSERT(_image_end <= 0x0801B000, "image runs into the vendor checksum page 0x0801B000")
    ASSERT(_enoinit + 1024 <= _estack, "less than 1 KB of RAM left for the stack")
    ASSERT(_estack < ORIGIN(RAM) + LENGTH(RAM), "initial SP at the top of RAM: the vendor bootloader faults on it")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
