/*
 * Board: M-Audio Keystation Mini 32 MK3 (Nuvoton NUC120LC1BN, 12 MHz HXT).
 *
 * Pins from research/devices/m-audio-keystation-mini32-mk3/analysis/hw-map.md
 * (static analysis of the stock firmware). LEDs active-low and Oct+ on PA4
 * confirmed on hardware (M1, 2026-10-30). Key matrix and knob: not yet.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_BOARD_KEYSTATION_MINI32_MK3_H
#define GEMIFI_BOARD_KEYSTATION_MINI32_MK3_H

#include <stdint.h>
#include "nuc1xx.h"

/* BOARD_ID, BOARD_NAME and the memory layout come from the board profile,
 * keystation-mini32-mk3.json, through the generated board_layout.h. */
#include "board_layout.h"

/* ---- buttons: PA1..PA7, direct GPIO, active-low, external pull-ups ---- */
#define BTN_SUSTAIN    1
#define BTN_EDIT       2
#define BTN_OCT_DOWN   3
#define BTN_OCT_UP     4
#define BTN_PITCH_DOWN 5
#define BTN_PITCH_UP   6
#define BTN_MOD        7
#define BOARD_BTN_FIRST 1
#define BOARD_BTN_LAST  7
#define BOARD_BTN_MASK  0x00FEu         /* PA1..PA7 */

/* Bootloader entry combo (sysex.md 6): Pitch-down + Pitch-up held. */
#define BOARD_BOOT_COMBO ((1u << BTN_PITCH_DOWN) | (1u << BTN_PITCH_UP))

/* ---- LEDs on port A, active-low ------------------------------------- */
/* Octave LEDs are bicolour: one colour on a static GPIO (PA10/PA11), the
 * other on a PWM pin (PA14/PA15), used here as plain GPIO. We drive both
 * colours together so the LED lights whichever colour is fitted. */
#define LED_OCT_DOWN  ((1u << 10) | (1u << 14))
#define LED_OCT_UP    ((1u << 11) | (1u << 15))
#define LED_SUSTAIN   (1u << 12)
#define LED_EDIT      (1u << 13)
#define LED_ALL       0xFC00u           /* PA10..PA15 */
#define BOARD_LED_ACTIVE_LOW 1

static inline void board_gpio_init(void)
{
    /* Port A: PA0..PA7 inputs (PA0 = volume pot, later ADC0), PA8/PA9 key
     * matrix drive lines (push-pull, idle low), PA10..PA15 LEDs (push-pull,
     * high = off). DOUT first so outputs come up at their idle level. */
    GPIO_DOUT(PORTA) = 0xFFFFu & ~((1u << 8) | (1u << 9));
    GPIO_PMD(PORTA) = 0x55550000u;
    /* Port B: matrix sense lines PB0..PB5, PB8, PB12 are plain inputs
     * (external pull-downs; never quasi-bidirectional). PB15 untouched. */
    GPIO_PMD(PORTB) &= ~(0x0FFFu | (3u << 16) | (3u << 24));
    /* Port C: drive lines PC0..PC3, PC6, PC7 push-pull, idle low. */
    GPIO_DOUT(PORTC) &= ~0x00CFu;
    GPIO_PMD(PORTC) = (GPIO_PMD(PORTC) & ~0xF0FFu) | 0x5055u;
}

/* Bit n set = button on PAn pressed. */
static inline uint32_t board_buttons(void)
{
    return ~GPIO_PIN(PORTA) & BOARD_BTN_MASK;
}

static inline void board_led(uint32_t mask, int on)
{
    if (on)
        GPIO_DOUT(PORTA) &= ~mask;      /* active-low */
    else
        GPIO_DOUT(PORTA) |= mask;
}

/* The key matrix, knob and LED behaviour live in the device description
 * (devices/keystation-mini32-mk3.yaml), used by the midi-controller
 * firmware. This header is for the bootloader and the small test apps. */

#endif
