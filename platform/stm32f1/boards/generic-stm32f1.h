/*
 * Board header for generic STM32F1 DIY boards (generic-stm32f103c8.json and
 * profiles copied from it): nothing on the board needs code at reset. The
 * layout, BOARD_ID / BOARD_NAME and the board pins (update LED, any USB
 * pull-up, soft power or recovery combo) come from the board profile through
 * the generated board_layout.h; recovery.c, usbd.c, the bootloader and the
 * midi-controller use them where they are defined. The buttons, LEDs and
 * everything else are the device YAML's (midi-controller).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_BOARD_GENERIC_STM32F1_H
#define GEMIFI_BOARD_GENERIC_STM32F1_H

#include <stdint.h>
#include "stm32f1.h"
#include "board_layout.h"

#define BOARD_BTN_FIRST 0
#define BOARD_BTN_LAST  0
#define BOARD_BOOT_COMBO 0u

#ifdef BOARD_POWER_LATCH_PIN
#error "a board with soft power needs its own header: board_gpio_init() must latch the power at reset"
#endif

/* Called from Reset_Handler before anything else: register-only. */
static inline void board_gpio_init(void)
{
}

/* Buttons are the device YAML's: none here. */
static inline uint32_t board_buttons(void)
{
    return 0;
}

static inline void board_led(uint32_t mask, int on)
{
    (void)mask;
    (void)on;
}

#endif
