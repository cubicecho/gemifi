/*
 * app.ld: a gemifi application on an STM32F1, above the bootloader (gemifi's,
 * or M-Audio's on the Code 61).
 *
 * The board profile (platform/stm32f1/boards/<board>.json) sets the layout;
 * board_app.ld (generated into the build's board/ dir, found through -L)
 * holds the MEMORY regions and the BOARD_* symbols asserted on below:
 *   BOARD_APP_BASE   this image: vectors 0x000-0x12F (16 + 60 entries), the
 *                    .gfw header at +0x130 (protocol/sysex.md 5), then code
 *   BOARD_APP_END    the image ends below it (Code 61: the vendor "app
 *                    valid" page 0x0801B000, whole-image checksum at
 *                    0x0801B3FE, written by the host packer)
 *   then the config pages and the update-request flag page (recovery.h),
 *   and BOARD_MAILBOX: the SRAM mailbox, the top 16 bytes of RAM (sys.h).
 * Code 61 map: research/devices/m-audio-code61/analysis/custom-firmware-path.md 4.
 *
 * The initial SP must NOT be the top of RAM. The vendor bootloader loads
 * MSP from our vector table and then accesses memory at the new SP itself
 * (seen over SWD: precise bus fault, BFAR = 0x20005000, in the bootloader
 * at 0x08000770, before our Reset_Handler ran). With SP = 0x20005000 that
 * is past the end of RAM, so the bootloader hard-faults and the board looks
 * dead. The stock app's SP is 0x200041A0. Keep BOARD_STACK_GUARD bytes free
 * above the initial SP for whatever the bootloader touches there.
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

    _estack = ORIGIN(RAM) + LENGTH(RAM) - BOARD_STACK_GUARD;
    _image_end = LOADADDR(.data) + SIZEOF(.data);

    ASSERT(ADDR(.text) == BOARD_APP_BASE, "image must start at BOARD_APP_BASE")
    ASSERT(_vectors_end <= BOARD_APP_BASE + 0x130, "vector table overlaps the image header")
    ASSERT(_gemifi_hdr_end == BOARD_APP_BASE + 0x150, "image header missing or not 32 bytes")
    ASSERT(_image_end <= BOARD_APP_END, "image runs past the app area (BOARD_APP_END; Code 61: the vendor checksum page)")
    ASSERT(_enoinit + 1024 <= _estack, "less than 1 KB of RAM left for the stack")
    ASSERT(_estack <= BOARD_MAILBOX, "the stack top runs into the SRAM mailbox (sys.h)")
    ASSERT(_estack < ORIGIN(RAM) + LENGTH(RAM), "initial SP at the top of RAM: the vendor bootloader faults on it")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
