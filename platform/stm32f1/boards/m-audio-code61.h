/*
 * Board: M-Audio Code 61 (Code 49/25 run the same firmware). Most likely a
 * medium-density STM32F103RB or GD32F103RB: 128 KB flash, 1 KB pages,
 * 20 KB RAM. HSE 8 MHz (stock: PLL x 9 = 72 MHz, USB / 1.5).
 *
 * Source: research/devices/m-audio-code61/analysis/hw-map.md (static
 * analysis of the Code 49 v1.2.1 app; nothing confirmed on hardware yet):
 *   - key matrix: rows through 3-to-8 decoders, PB3..PB5 = address,
 *     PB6/PB7 = group (PB6: rows 0-7, PB7: rows 8-15, both low: rows
 *     16-22); columns PB8..PB15, pull-up, active-low. Panel buttons are
 *     rows 16-20, encoders rows 21-22.
 *   - LEDs and display: two TM1628-class serial LED drivers (CLK PC10, DIO
 *     PC11 open-drain, STB PA15 / PC12). No driver yet: board_led() is a
 *     no-op stub.
 *   - USB D+ pull-up switched by PA8, active high (BOARD_USB_PULLUP_PIN).
 *   - PB3, PB4 and PA15 are JTAG pins at reset: BOARD_SWJ_NOJTAG makes
 *     sys_clock_init() switch JTAG off (SWD stays on).
 * The midi-controller takes the rest from devices/m-audio-code61.yaml.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_BOARD_M_AUDIO_CODE61_H
#define GEMIFI_BOARD_M_AUDIO_CODE61_H

#include <stdint.h>
#include "stm32f1.h"

#define BOARD_ID    0x0002              /* protocol/sysex.md section 7 */
#define BOARD_NAME  "Code 61"

/* ---- flash ------------------------------------------------------------ */
/* 1 KB erase pages (the vendor update erases in 0x400 steps and its
 * settings writer is 1 KB aligned). The probe app logs the size register. */
#define BOARD_FLASH_PAGE 1024u

/* Profile journal (config_store.c): pages inside the vendor "EEPROM" area
 * 0x0801B400-0x0801FBFF, outside the checksummed app range.
 * 0x0801EC00 and 0x0801F000: the pages the research draft (devices/
 * m-audio-code61.yaml, hw-map.md) found the stock app never touches, so
 * its per-unit calibration survives and a return to stock finds its own
 * pages intact. Not yet confirmed on hardware. */
#define BOARD_CONFIG_BASE  0x0801EC00u
#define BOARD_CONFIG_PAGES 2u
/* (0x0801F800 is the vendor's preset-names page: never use it.) These two
 * pages may hold old data on a real unit; config_store only trusts
 * magic + CRC records and erases a page before writing into it. */

/* ---- pins the platform drives ------------------------------------------ */
#define BOARD_USB_PULLUP_PIN 8          /* PA8, high = D+ pull-up on (usbd.c) */
#define BOARD_SWJ_NOJTAG     1          /* free PB3/PB4/PA15, keep SWD */

/* ---- recovery combo (src/recovery.c, custom-firmware-path.md 5) ------- */
/* Held at reset: write the vendor update-request flag (0x0801FC00) and
 * reset into the vendor bootloader, before any other gemifi code runs.
 *
 * No panel button is on a plain GPIO; they sit in key-matrix row 16:
 * drive PB3..PB7 low (decoder address 0, both group selects low = row 16),
 * pull up the columns PB8..PB15, and read PB8 and PB9: both low = held.
 * The labels of those two buttons are UNVERIFIED (most likely Octave - and
 * Octave +); confirm on hardware. */
#define BOARD_RECOVERY_SWJ_NOJTAG  1                /* PB3/PB4 are JTAG at reset */
#define BOARD_RECOVERY_DRIVE_PORT  PORTB
#define BOARD_RECOVERY_DRIVE_MASK  (0x1Fu << 3)     /* PB3..PB7 */
#define BOARD_RECOVERY_DRIVE_LEVEL 0
#define BOARD_RECOVERY_PORT        PORTB
#define BOARD_RECOVERY_PULL_MASK   0xFF00u          /* PB8..PB15 pull-up */
#define BOARD_RECOVERY_MASK        ((1u << 8) | (1u << 9))
#define BOARD_RECOVERY_ACTIVE      0                /* pressed = low */

/* ---- buttons / LEDs (matrix / TM1628: not in the board header) ------ */
#define BOARD_BTN_FIRST 0
#define BOARD_BTN_LAST  0
#define BOARD_BOOT_COMBO 0u

/* Soft power (verified on hardware over SWD, 2026-11-03):
 *   PC13 = power button, input pull-up, LOW while pressed;
 *   PC15 = power latch, push-pull, HIGH = stay on.
 * Pressing the button powers the board only while it is held; the firmware
 * must drive PC15 high to keep it on after release. The stock app's power-off
 * path (hold the button ~2 s: save settings, then PC15 = 0 at 0x08010710)
 * drives it low to switch off. The vendor bootloader used to latch it before
 * the app ran; with it gone (the RDP unlock erased it) the app must.
 *
 * Called from Reset_Handler before anything else (startup.c), so it must
 * stay register-only: no globals, no calls. The matrix and LED drivers are
 * the device config's (midi-controller) business. */
#define BOARD_POWER_BTN_PIN   13            /* PC13, low = pressed */
#define BOARD_POWER_LATCH_PIN 15            /* PC15, high = stay on */

static inline void board_gpio_init(void)
{
    RCC_APB2ENR |= RCC_APB2_IOP(PORTC);
    GPIO_BSRR(PORTC) = 1u << 15;                       /* PC15 high: stay on */
    GPIO_CRH(PORTC) = (GPIO_CRH(PORTC) & ~(0xFu << 28)) | GPIO_CFG_OUT_PP << 28;
    GPIO_BSRR(PORTC) = 1u << 13;                       /* PC13 pull-up */
    GPIO_CRH(PORTC) = (GPIO_CRH(PORTC) & ~(0xFu << 20)) | GPIO_CFG_PULL << 20;
}

/* Bit n set = button n pressed. Buttons are in the matrix: none here. */
static inline uint32_t board_buttons(void)
{
    return 0;
}

/* LEDs are behind the TM1628-class drivers: no driver yet, no-op. */
static inline void board_led(uint32_t mask, int on)
{
    (void)mask;
    (void)on;
}

#endif
