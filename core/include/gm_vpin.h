/*
 * gm_vpin.h: virtual pins (gm_device.h): MCU pins through the HAL, expander
 * hubs in software.
 *
 * Levels are *logical*: 1 = active (pressed, lit, row driven), with
 * GM_PIN_INVERT applied. Hub I/O is cached:
 *   - HC165 / MCP23017 / PCF8574 inputs are read by vpin_refresh();
 *   - HC595 / MCP23017 / PCF8574 outputs are written by vpin_flush(),
 *     TM1628 outputs by vpin_flush_tm1628();
 *   - switch-matrix hubs are scanned by vpin_scan_matrices() (scan tick).
 * A decoder hub's output is written straight away: "on" selects it.
 * Shift-register hubs are refreshed/flushed from the scan tick (fast GPIO);
 * I2C hubs only from the main loop (vpin_refresh_i2c / vpin_flush_i2c),
 * because I2C can't run in an interrupt on most platforms. TM1628 hubs are
 * also written from the main loop (vpin_flush_tm1628): a whole chip takes
 * a few hundred us of bit-banging, too long for the scan tick.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_VPIN_H
#define GEMIFI_GM_VPIN_H

#include <stdint.h>
#include "gm_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the hub control pins and reset the caches. */
void vpin_init(void);

void vpin_input(gm_pin_t p);       /* MCU pins: set mode from the pull flags */
void vpin_output(gm_pin_t p);      /* and drive it inactive                 */

int vpin_read(gm_pin_t p);         /* cached for hubs; mux: switches channel */
void vpin_write(gm_pin_t p, int active);
int32_t vpin_analog(gm_pin_t p);   /* 0..4095, or -1 (not analog-capable)   */

int vpin_on_i2c(gm_pin_t p);       /* pin lives on an I2C hub               */
int vpin_is_none(gm_pin_t p);

void vpin_refresh(void);           /* shift-register inputs                  */
void vpin_flush(void);             /* shift-register outputs (if changed)    */
void vpin_refresh_i2c(void);
void vpin_flush_i2c(void);
int vpin_has_matrices(void);       /* any switch-matrix hub                  */
int vpin_is_decoder(gm_pin_t p);   /* a decoder output: "off" does nothing  */
/* A decoder output that is selected by one store: *word to *reg. 0 if p
 * isn't one (selected pin by pin through vpin_write instead). */
int vpin_select_word(gm_pin_t p, volatile uint32_t **reg, uint32_t *word);
/* Where p's level can be read with one load: *mask's bit of the returned
 * word, electrical (apply GM_PIN_INVERT). MCU pins (hal_pin_in_reg) and
 * cached hub inputs; NULL for a mux channel or without a register. */
const volatile uint32_t *vpin_in_reg(gm_pin_t p, uint32_t *mask);
void vpin_scan_matrices(void);     /* scan them (from the scan tick)         */
void vpin_flush_tm1628(void);      /* TM1628 RAM (if changed)                */

#ifdef __cplusplus
}
#endif

#endif
