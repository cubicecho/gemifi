/*
 * board.h: pulls in the board selected by the build (BOARD=... in make,
 * which defines BOARD_HEADER). Each board header provides BOARD_ID,
 * BOARD_NAME, BOARD_BOOT_COMBO, board_gpio_init(), board_buttons() and
 * board_led(); see boards/keystation-mini32-mk3.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_BOARD_H
#define GEMIFI_BOARD_H

#ifndef BOARD_HEADER
#define BOARD_HEADER "keystation-mini32-mk3.h"
#endif
#include BOARD_HEADER

#endif
