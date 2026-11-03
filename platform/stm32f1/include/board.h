/*
 * board.h: pulls in the board selected by the build (BOARD=... in make,
 * which defines BOARD_HEADER). Each board header provides BOARD_ID,
 * BOARD_NAME, BOARD_FLASH_PAGE, BOARD_CONFIG_BASE/PAGES, the recovery combo
 * (BOARD_RECOVERY_* or BOARD_RECOVERY_NONE), board_gpio_init(),
 * board_buttons() and board_led(); see boards/m-audio-code61.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_BOARD_H
#define GEMIFI_BOARD_H

#ifndef BOARD_HEADER
#define BOARD_HEADER "m-audio-code61.h"
#endif
#include BOARD_HEADER

#endif
