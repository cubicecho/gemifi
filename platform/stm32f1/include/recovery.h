/*
 * recovery.h: the vendor-bootloader update request and the reset-time
 * recovery combo (research/devices/m-audio-code61/analysis/
 * custom-firmware-path.md 3 and 5).
 *
 * The stock app asks the vendor bootloader for update mode by writing an
 * 8-byte magic to the start of the last 1 KB flash page (0x0801FC00, rest
 * of the page FF). gemifi keeps that page on every board: BOARD_FLAG_PAGE
 * in the board profile (the last flash page unless it says otherwise). The bootloader checks it at power-up, stays in update
 * mode, and clears it at the end of an update. gemifi does the same, then
 * resets (whether a soft reset is enough, or a power cycle is needed, is
 * still to be tested on hardware).
 *
 * src/recovery.c depends on nothing but registers (no .data, no .bss, no
 * other gemifi code), because recovery_check() runs first in Reset_Handler.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_RECOVERY_H
#define GEMIFI_RECOVERY_H

#include <stdint.h>
#include "board_layout.h"

#define UPDATE_FLAG_ADDR BOARD_FLAG_PAGE
/* bytes 55 5A A5 AA 59 95 3A A3, as little-endian halfwords */
#define UPDATE_FLAG_H0   0x5A55u
#define UPDATE_FLAG_H1   0xAAA5u
#define UPDATE_FLAG_H2   0x9559u
#define UPDATE_FLAG_H3   0xA33Au

/* Called from Reset_Handler before .data/.bss/clocks. If the board's
 * recovery combo (BOARD_RECOVERY_*) is held, writes the update request and
 * resets; otherwise restores the pins it touched and returns. */
void recovery_check(void);

/* Write the update request (erase the flag page, program the magic) unless
 * it is already there. Returns 0 on success, -1 on a flash error. Leaves
 * the flash locked. Does not reset. */
int recovery_request_update(void);

/* 1 if the update-request magic is at UPDATE_FLAG_ADDR. */
int recovery_update_requested(void);

#endif
