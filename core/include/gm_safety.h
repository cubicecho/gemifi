/*
 * gm_safety.h: recovery and safety (core/README.md "Factory-reset combo",
 * "Physical unlock", "Status LED"; protocol/sysex.md 4.7, 4.8).
 *
 *   - The factory-reset combo: buttons held at power-up for a few seconds
 *     wipe the stored settings (not the firmware).
 *   - Physical unlock: FW_BEGIN, REBOOT(1) and FACTORY_RESET reply LOCKED
 *     until the user holds the unlock combo while the unlock window is open.
 *   - Blink codes on the status LED.
 *   - The diagnostics the diag.* params show.
 *
 * Each part compiles out when the device YAML doesn't ask for it
 * (GM_RESET_COMBO 0, GM_UNLOCK 0, GM_STATUS_LED -1); the diagnostics are
 * always there.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_SAFETY_H
#define GEMIFI_GM_SAFETY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* diag.reset_reason values (sysex.md 4.8) */
enum {
    GM_RESET_UNKNOWN, GM_RESET_POWER_ON, GM_RESET_PIN, GM_RESET_SOFTWARE,
    GM_RESET_WATCHDOG, GM_RESET_BROWN_OUT, GM_RESET_CRASH, GM_RESET_LOW_POWER,
};
/* diag.settings values */
enum {
    GM_SETTINGS_LOADED, GM_SETTINGS_NONE, GM_SETTINGS_REFUSED, GM_SETTINGS_SAFE, GM_SETTINGS_WIPED,
};

/* The diag.* params' values, in param order (diag.reset_reason ... uptime).
 * The core refreshes them every main-loop pass, so a params_defaults()
 * (FACTORY_RESET) can't clear them. */
#define GM_DIAG_N 7
extern int32_t gm_diag_param[GM_DIAG_N];

/* Platform -> core: one scan (the timer tick) or one main-loop pass took
 * us; the longest since power-up is kept. Safe from an interrupt. */
extern volatile uint32_t gm_diag_scan_max, gm_diag_loop_max;
static inline void gm_diag_scan_us(uint32_t us)
{
    if (us > gm_diag_scan_max)
        gm_diag_scan_max = us;
}
static inline void gm_diag_loop_us(uint32_t us)
{
    if (us > gm_diag_loop_max)
        gm_diag_loop_max = us;
}

/* The diag.* values into gm_diag_param (every gm_poll pass; after a
 * params_defaults(), e.g. FACTORY_RESET, at once). */
void gm_diag_refresh(void);

/* ---- core internal ------------------------------------------------------
 * gm_init: after the settings are loaded (settings = GM_SETTINGS_*). */
void gm_safety_init(int settings);
/* gm_poll, every pass: held = the debounced buttons (bit = index). */
void gm_safety_poll(uint32_t now, uint64_t held);
/* buttons_sample, for every press and release before anything else sees
 * it: 1 = it belongs to a combo right now (the unlock window is open, or
 * the factory-reset combo is being held at power-up), so it does nothing
 * else. A release is swallowed exactly when its press was. */
int gm_safety_button(uint32_t i, int down);
/* led_set: the status LED's level while a blink code shows (0 or 1), or
 * -1 for its normal level. */
int gm_status_level(void);
/* leds_update: 1 while a blink code shows (the status LED is redrawn every
 * LED update meanwhile). */
int gm_status_active(void);

/* ---- app_proto (sysex.md 4.7) ------------------------------------------- */
/* 1 = locked: reply LOCKED (the unlock window opens or restarts).
 * 0 = go ahead. Always 0 without GM_UNLOCK. */
int gm_unlock_check(void);
/* Any request: the unlocked state's idle timer restarts. */
void gm_unlock_activity(void);
/* UNLOCK's state (0 locked, 1 unlocked) and the window's seconds left;
 * open = 1 also opens (or restarts) the window when locked. */
uint32_t gm_unlock_state(int open, uint32_t *window_s);

#ifdef __cplusplus
}
#endif

#endif
