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
    P_PRESET_ACTIVE = 17,
    P_PRESET_PROGRAM = 18,
    P_PRESET_DIRTY = 19,
    /* pads (GM_N_PADS > 0) */
    P_PAD_NOTE = 20,
    P_PAD_CHANNEL = 21,
    P_PAD_CURVE = 22,
    P_PAD_AFTERTOUCH = 23,
    P_PAD_THRESHOLD = 24,
    P_PAD_RELEASE = 25,
    P_PAD_FULL = 26,
    P_PAD_WINDOW = 27,
    P_PAD_RETRIGGER = 28,
    P_ANALOG_MODE = 29,
    P_DIN_THRU = 30,                 /* din_midi: (GM_DIN) */
    P_DIN_OUT = 31,
    P_DIN_USB = 32,                  /* din_midi.mode usb_merge only */
};

#ifndef GM_N_PADS
#define GM_N_PADS 0                  /* a config generated before pads */
#endif
#ifndef GM_DIN
#define GM_DIN 0                     /* no din_midi: */
#endif

enum { CURVE_NORMAL, CURVE_SOFT, CURVE_HARD, CURVE_FIXED };

/* analog.mode values */
enum { AN_MODE_CC, AN_MODE_BEND, AN_MODE_PRESSURE };

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
    ACT_PRESET_NEXT,                 /* 13 */
    ACT_PRESET_PREV,                 /* 14 */
    ACT_PRESET_SELECT,               /* 15: button.number = slot 1..GM_N_PRESETS */
    ACT_COUNT
};
/* Mackie Control button (button.number = its MCU note), 16. Only on devices
 * with mcu: (GM_MCU), where it is offered as one more action (the option
 * list stays contiguous: it ends at 15 elsewhere). Was 13 before presets. */
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
    int32_t analog_mode[GM_ARR(GM_N_ANALOGS)];   /* AN_MODE_* */
    int32_t btn_action[GM_ARR(GM_N_BUTTONS)];
    int32_t btn_number[GM_ARR(GM_N_BUTTONS)];
    int32_t enc_cc[GM_ARR(GM_N_ENCODERS)];
    int32_t enc_mode[GM_ARR(GM_N_ENCODERS)];
    int32_t bend_glide;              /* ms, 0 = jump */
    int32_t ramp_time;               /* ms, 0 = jump */
    int32_t events;
    int32_t log_level;               /* 0 off .. 4 debug */
    int32_t preset_active;           /* 1..GM_N_PRESETS (global)       */
    int32_t preset_program;          /* -1 none, 0..127 (per preset)   */
    int32_t preset_dirty;            /* volatile: unsaved host edits   */
#if GM_DIN
    int32_t din_thru;                /* DIN In -> DIN Out              */
    int32_t din_out;                 /* the controller -> DIN Out      */
    int32_t din_usb;                 /* usb_merge: host -> DIN Out     */
#endif
#if GM_N_PADS > 0                    /* no RAM on devices without pads */
    int32_t pad_note[GM_N_PADS];
    int32_t pad_channel;           /* 1..16; 0 = midi.channel        */
    int32_t pad_curve;               /* CURVE_*                        */
    int32_t pad_aftertouch;          /* 1: poly pressure while held    */
    int32_t pad_threshold;           /* reading 0..4095 that starts a hit */
    int32_t pad_release;             /* below this the pad is let go   */
    int32_t pad_full;                /* peak reading for velocity 127  */
    int32_t pad_window;              /* ms: peak search after the threshold */
    int32_t pad_retrigger;           /* ms after a release with no new hit */
#endif
} gm_settings_t;

extern gm_settings_t gm_cfg;

/* Fills the index-label tables from gm_device; call before params_*. */
void gm_settings_init(void);

#endif
