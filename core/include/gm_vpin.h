/*
 * gm_vpin.h: virtual pins (gm_device.h): MCU pins through the HAL, expander
 * hubs in software, each hub type a driver (core/src/gm_hub.h).
 *
 * Levels are *logical*: 1 = active (pressed, lit, row driven), with
 * GM_PIN_INVERT applied. Hub I/O is cached:
 *   - HC165 inputs are read by vpin_refresh(), HC595 outputs written by
 *     vpin_flush(), from the scan tick (fast GPIO): VPIN_SCAN_IO hubs;
 *   - MCP23017 / PCF8574 by vpin_refresh_i2c() / vpin_flush_i2c(), only from
 *     the main loop, because I2C can't run in an interrupt on most
 *     platforms: VPIN_BUS_IO hubs;
 *   - TM1628 and MAUDIO_PADS outputs by vpin_poll() on every main-loop pass
 *     (VPIN_MAIN hubs): only what changed, a few bytes per pass, since a
 *     whole TM1628 takes a few hundred us of bit-banging (too long for the
 *     scan tick, or one pass);
 *   - switch-matrix hubs are scanned by vpin_scan_matrices() (scan tick).
 * A decoder hub's output is written straight away: "on" selects it.
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
/* 1 once after an XY pad axis is touched again: the reading jumps there,
 * so a smoothing filter should start over from it. 0 for other pins. */
int vpin_analog_jump(gm_pin_t p);

int vpin_on_i2c(gm_pin_t p);       /* pin lives on an I2C hub (VPIN_BUS_IO) */
int vpin_is_none(gm_pin_t p);

/* What the hubs need from the controller: where their I/O runs, and what
 * their pins can do. vpin_caps() = every hub's, vpin_pin_caps(p) = p's
 * hub's (0 for an MCU pin or no pin). From the config: callable before
 * vpin_init. */
#define VPIN_SCAN_IO (1u << 0)     /* vpin_refresh / vpin_flush, in the scan tick */
#define VPIN_BUS_IO  (1u << 1)     /* vpin_refresh_i2c / vpin_flush_i2c, main loop */
#define VPIN_MAIN    (1u << 2)     /* vpin_poll, every main-loop pass          */
#define VPIN_MATRIX  (1u << 3)     /* a switch matrix: vpin_scan_matrices      */
#define VPIN_RGB     (1u << 4)     /* any colour per LED: vpin_rgb             */
uint32_t vpin_caps(void);
uint32_t vpin_pin_caps(gm_pin_t p);

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
/* Hardware scan (gm_controller.h gm_scan_plan_t): add every switch matrix's
 * rows to the plan (a row already in it is shared). 0, and the plan's rows
 * left as they were, if one can't join (its rows have no one-store select
 * on the plan's register, or its columns aren't bits of the plan's input
 * register). Struct-free on purpose: the plan's fields are passed in. */
int vpin_scan_plan(volatile uint32_t **sel_reg, uint32_t *sel_word, uint32_t *n_rows,
                   uint32_t max_rows, const volatile uint32_t **in_reg, uint32_t *in_mask);
/* From then on: 1 = switch matrices take their rows from vpin_scan_feed()
 * (one frame in plan order) and vpin_scan_matrices() skips them. */
void vpin_scan_hw(int on);
void vpin_scan_feed(const uint16_t *frame);
/* Buttons (controller.c buttons_sample): vpin_span_take() once per sample,
 * then vpin_read_span(p, &any) per button = 1 if p was active in every scan
 * of a switch matrix since the last take, and *any = 1 if it was active in
 * any of them. Other pins have no history: both are vpin_read(p). */
void vpin_span_take(void);
int vpin_read_span(gm_pin_t p, int *any);
/* Pads (controller.c pads_scan): vpin_analog_peaks() once per pass, then
 * vpin_analog_peak(p) per pad = its strongest reading (after INVERT) since
 * the last pass, with the hardware analog scan (gm_adc_plan); otherwise just
 * vpin_analog(p). */
void vpin_analog_peaks(void);
int32_t vpin_analog_peak(gm_pin_t p);
/* Main loop, every pass: each hub's poll (MAUDIO_PADS: one changed LED at
 * most every 5 ms, the keepalive every 50 ms), and what changed on the
 * bit-banged ones (TM1628), a few bus bytes (about 50 us) per call; a big
 * change goes out over a few passes. busy: this pass already did a lot (an
 * LED update): only the polls, no bus bytes. */
void vpin_poll(uint32_t now_ms, int busy);
/* Everything that changed on the main-loop hubs, now (the lamp test). */
void vpin_send_all(void);
/* The same as vpin_send_all / vpin_poll's bus part, by their old names. */
void vpin_flush_tm1628(void);
void vpin_poll_tm1628(void);
/* Set a whole colour (0xRRGGBB, 8 bits a colour) on the LED whose red pin
 * is p, if p is the red pin of a full-colour LED (VPIN_RGB; on MAUDIO_PADS,
 * number LED * 3): 1, else 0 (the caller shows it with vpin_write). */
int vpin_rgb(gm_pin_t p, uint32_t rgb);

/* TM1628 hubs with fade: (core/README.md "TM1628 LED drivers"): the chip
 * has one brightness (8 steps), so it shows the brightest of its LEDs.
 * vpin_fade notes that the LED on pin p is lit at v (0..255, as eyes see
 * it), colour = 1 if that is a host's colour; vpin_fade_apply then sets
 * each fade chip from the brightest host colour noted since the last call,
 * else the brightest other LED (a chip with none keeps its brightness). */
void vpin_fade(gm_pin_t p, uint32_t v, int colour);
void vpin_fade_apply(void);
/* 1 if p is on a TM1628 hub with fade: (vpin_fade only counts those). */
int vpin_fades(gm_pin_t p);

#ifdef __cplusplus
}
#endif

#endif
