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

/* The layout, the board pins (USB pull-up PA8, soft power PC13/PC15, the
 * recovery combo, JTAG off) and BOARD_ID / BOARD_NAME come from the board
 * profile, m-audio-code61.json, through the generated board_layout.h. */
#include "board_layout.h"

/* ---- buttons / LEDs (matrix / TM1628: not in the board header) ------ */
#define BOARD_BTN_FIRST 0
#define BOARD_BTN_LAST  0
#define BOARD_BOOT_COMBO 0u

/* Soft power (the profile's soft_power; verified on hardware over SWD, 2026-11-03):
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
_Static_assert(BOARD_POWER_LATCH_PORT == PORTC && BOARD_POWER_LATCH_PIN == 15 &&
               BOARD_POWER_BTN_PORT == PORTC && BOARD_POWER_BTN_PIN == 13,
               "board_gpio_init() below is written for PC15 (latch) and PC13 (button)");

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
