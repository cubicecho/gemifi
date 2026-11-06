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
    P_PRESET_COUNT = 33,             /* presets in use (GM_N_PRESETS > 1) */
    /* layers (GM_LAYER_PARAMS): global, stored once (core/README.md
     * "Button layers", "Cycles") */
    P_LAYER_ENABLED = 34,            /* per mode (index = mode index) */
    P_LAYER_BTN_ACTION = 35,         /* per button twin (GM_BTN_TWINS) */
    P_LAYER_BTN_NUMBER = 36,
    P_LAYER_ENC_CC = 37,             /* per encoder twin (GM_ENC_TWINS); -1 = none */
    P_LAYER_ENC_MODE = 38,
    /* read-only: the hash of the YAML the firmware was built from (GM_CONFIG_HASH,
     * core/README.md "Config hash") */
    P_CONFIG_HASH = 39,
    /* MIDI messages (core/README.md "MIDI messages"): 41-59 */
    P_PRESET_BANK = 41,              /* -1 none, 0..16383 (GM_PRESET_BANK) */
    /* diagnostics (sysex.md 4.10): READ_ONLY, VOLATILE; 72-74 kept for more */
    P_DIAG_RESET = 65,
    P_DIAG_BOOT_LOOP = 66,
    P_DIAG_SETTINGS = 67,
    P_DIAG_SCAN_MAX = 68,
    P_DIAG_LOOP_MAX = 69,
    P_DIAG_SCAN_PERIOD = 70,
    P_DIAG_UPTIME = 71,
};

#ifndef GM_N_PADS
#define GM_N_PADS 0                  /* a config generated before pads */
#endif
#ifndef GM_DIN
#define GM_DIN 0                     /* no din_midi: */
#endif
#ifndef GM_N_MODES
#define GM_N_MODES 0                 /* a config generated before modes */
#endif
#ifndef GM_LAYERS
#define GM_LAYERS 0                  /* layer twins (all kinds) */
#endif
#ifndef GM_BTN_TWINS
#define GM_BTN_TWINS 0               /* the last GM_BTN_TWINS buttons are twins */
#endif
#ifndef GM_ENC_TWINS
#define GM_ENC_TWINS 0               /* the last GM_ENC_TWINS encoders are twins */
#endif
#ifndef GM_N_CYCLES
#define GM_N_CYCLES 0                /* modes: cycle: groups */
#endif
/* layer.enabled (one per mode) on a device with layer twins or a cycle. */
#define GM_LAYER_PARAMS (GM_N_MODES > 0 && (GM_LAYERS > 0 || GM_N_CYCLES > 0))
/* The YAML's own buttons / encoders: button.* / encoder.* (per preset). The
 * twins after them share the arrays below, under the layer.* params. */
#define GM_N_OWN_BUTTONS (GM_N_BUTTONS - GM_BTN_TWINS)
#define GM_N_OWN_ENCODERS (GM_N_ENCODERS - GM_ENC_TWINS)

/* MIDI messages (core/README.md "MIDI messages"): each is compiled in only
 * when the YAML uses it, or lists it in midi_messages.enable. */
#ifndef GM_MSG_CC14
#define GM_MSG_CC14 0                /* analog / encoder mode cc14 */
#endif
#ifndef GM_MSG_NRPN
#define GM_MSG_NRPN 0                /* modes nrpn, nrpn14, rpn, rpn14 (numbers 0..16383) */
#endif
#ifndef GM_NULL_RPN
#define GM_NULL_RPN 0                /* midi_messages.null_rpn: 101/100 = 127 after each */
#endif
#ifndef GM_MSG_PROGRAM
#define GM_MSG_PROGRAM 0             /* button action program; bank select (button bank:, preset.bank) */
#endif
#ifndef GM_MSG_PANIC
#define GM_MSG_PANIC 0               /* button action panic */
#endif
#ifndef GM_MSG_MMC
#define GM_MSG_MMC 0                 /* button action mmc */
#endif
#ifndef GM_MMC_DEVICE
#define GM_MMC_DEVICE 0x7F           /* midi_messages.mmc_device_id */
#endif
#ifndef GM_N_SYSEX
#define GM_N_SYSEX 0                 /* midi_messages.sysex: gm_sysex[] */
#endif
#define GM_MSG_WIDE (GM_MSG_CC14 || GM_MSG_NRPN)   /* 14-bit values */
/* preset.bank: with presets, a preset's Bank Select before its Program Change */
#define GM_PRESET_BANK (GM_MSG_PROGRAM && GM_N_PRESETS > 1)

enum { CURVE_NORMAL, CURVE_SOFT, CURVE_HARD, CURVE_FIXED };

/* analog.mode values. 3..8 only with the message kinds they need. */
enum {
    AN_MODE_CC, AN_MODE_BEND, AN_MODE_PRESSURE,
    AN_MODE_CC14,                    /* GM_MSG_CC14 */
    AN_MODE_NRPN, AN_MODE_NRPN14, AN_MODE_RPN, AN_MODE_RPN14,   /* GM_MSG_NRPN */
    AN_MODE_SYSEX,                   /* GM_N_SYSEX: analog.cc = the SysEx index */
};
/* What a value goes out as (controller.c send_value): the analog modes from
 * cc14 on are AN_MODE_* - 2, the encoder modes from cc14 on ENC_* - 3. */
enum { K_CC, K_CC14, K_NRPN, K_NRPN14, K_RPN, K_RPN14, K_SYSEX };
#define GM_N_AN_MODES (GM_N_SYSEX ? 9 : GM_MSG_NRPN ? 8 : GM_MSG_CC14 ? 4 : 3)

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
 * with mcu: (GM_MCU), where it is offered as one more action. Was 13 before
 * presets. */
#define ACT_MCU ACT_COUNT
/* The MIDI message actions, each only with its GM_MSG_* (else its option is
 * "": params_set refuses it, and editors leave it out). */
#define ACT_PROGRAM 17               /* button.number = program 0..127 */
#define ACT_PANIC   18
#define ACT_MMC     19               /* button.number = MMC command (stop 1, play 2, ...) */
#define ACT_SYSEX   20               /* button.number = SysEx index */

enum {
    ENC_ABSOLUTE, ENC_REL_TWOS, ENC_REL_OFFSET,
    ENC_REL_SIGN,                    /* sign-magnitude: 1..63 up, 65..127 down */
    ENC_CC14,                        /* GM_MSG_CC14: absolute 0..16383 */
    ENC_NRPN, ENC_NRPN14, ENC_RPN, ENC_RPN14,   /* GM_MSG_NRPN: absolute */
};
#define GM_N_ENC_MODES (GM_MSG_NRPN ? 9 : GM_MSG_CC14 ? 5 : 4)

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
    /* buttons, then their twins: button.* is [0, GM_N_OWN_BUTTONS), the
     * twins' layer.button.* the rest; encoders likewise (enc_cc -1 = none) */
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
    int32_t preset_count;            /* presets in use: slots 1..count */
#if GM_PRESET_BANK
    int32_t preset_bank;             /* -1 none, 0..16383 (per preset) */
#endif
#if GM_DIN
    int32_t din_thru;                /* DIN In -> DIN Out              */
    int32_t din_out;                 /* the controller -> DIN Out      */
    int32_t din_usb;                 /* usb_merge: host -> DIN Out     */
#endif
#if GM_LAYER_PARAMS
    int32_t layer_enabled[GM_N_MODES];   /* 0: a cycle skips the step, a toggle / hold can't enter */
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

/* Call before params_*. Nothing to set up any more: the index labels are
 * the builder's tables in flash (gm_device.h gm_*_names). */
static inline void gm_settings_init(void)
{
}

#endif
