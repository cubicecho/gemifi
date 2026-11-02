/*
 * gm_settings.h: the controller's config profile. Its params (src/settings.c)
 * are sized from the device's component counts (gm_device_config.h), so a
 * device without encoders simply has no encoder params.
 *
 * Param ids are stable forever: never renumber, never reuse a removed one.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_SETTINGS_H
#define GEMIFI_GM_SETTINGS_H

#include <stdint.h>
#include "gm_device_config.h"

/* Arrays need at least one element even when a device has none. */
#define GM_ARR(n) ((n) > 0 ? (n) : 1)

enum {
    P_CHANNEL = 1,
    P_TRANSPOSE = 2,
    P_OCTAVE = 3,
    P_VEL_CURVE = 4,
    P_VEL_FIXED = 5,
    P_VEL_FAST = 6,
    P_VEL_SLOW = 7,
    P_ANALOG_CC = 8,
    P_BTN_ACTION = 9,
    P_BTN_NUMBER = 10,
    P_BEND_GLIDE = 11,
    P_RAMP_TIME = 12,
    P_EVENTS = 13,
    P_LOG_LEVEL = 14,
    P_ENC_CC = 15,
    P_ENC_MODE = 16,
};

enum { CURVE_NORMAL, CURVE_SOFT, CURVE_HARD, CURVE_FIXED };

/* button.action values (core/README.md "Button actions") */
enum {
    ACT_NONE,
    ACT_CC_MOMENTARY,
    ACT_CC_TOGGLE,
    ACT_CC_RAMP,
    ACT_NOTE,
    ACT_OCTAVE_DOWN,
    ACT_OCTAVE_UP,
    ACT_TRANSPOSE_DOWN,
    ACT_TRANSPOSE_UP,
    ACT_BEND_DOWN,
    ACT_BEND_UP,
    ACT_PROGRAM_NEXT,
    ACT_PROGRAM_PREV,
    ACT_COUNT
};
/* Mackie Control button (button.number = its MCU note). Only on devices with
 * mcu: (GM_MCU), where it is offered as one more action. */
#define ACT_MCU ACT_COUNT

enum { ENC_ABSOLUTE, ENC_REL_TWOS, ENC_REL_OFFSET };

typedef struct {
    int32_t channel;                 /* 1..16 */
    int32_t transpose;               /* semitones */
    int32_t octave;
    int32_t vel_curve;
    int32_t vel_fixed;
    int32_t vel_fast;                /* 0.1 ms: this fast or faster = 127 */
    int32_t vel_slow;                /* 0.1 ms: this slow or slower = 1   */
    int32_t analog_cc[GM_ARR(GM_N_ANALOGS)];
    int32_t btn_action[GM_ARR(GM_N_BUTTONS)];
    int32_t btn_number[GM_ARR(GM_N_BUTTONS)];
    int32_t enc_cc[GM_ARR(GM_N_ENCODERS)];
    int32_t enc_mode[GM_ARR(GM_N_ENCODERS)];
    int32_t bend_glide;              /* ms, 0 = jump */
    int32_t ramp_time;               /* ms, 0 = jump */
    int32_t events;
    int32_t log_level;               /* 0 off .. 4 debug */
} gm_settings_t;

extern gm_settings_t gm_cfg;

/* Fills the index-label tables from gm_device; call before params_*. */
void gm_settings_init(void);

#endif
