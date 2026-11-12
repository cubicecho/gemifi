/*
 * settings.c: the controller's param table (the schema an editor reads,
 * protocol/sysex.md 4.4).
 *
 * Scope (research/core/presets-design.md §1): PF_PRESET marks what each
 * preset has its own value of, the mapping (what a control sends). The rest
 * is global: the hardware side (velocity calibration), debug, preset.active.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_settings.h"
#include "gm_device.h"
#include "params.h"
#include "gm_safety.h"

gm_settings_t gm_cfg;
int32_t gm_diag_param[GM_DIAG_N];  /* the diag.* values (gm_safety.h): safety.c fills them in */

/* Key-time defaults, in 0.1 ms. A device sets its own in its YAML
 * (key_matrix.velocity), since they depend on the keybed. */
#ifndef GM_VEL_FAST_DEFAULT
#define GM_VEL_FAST_DEFAULT 12
#endif
#ifndef GM_VEL_SLOW_DEFAULT
#define GM_VEL_SLOW_DEFAULT 1200
#endif

/* Pad defaults: the YAML's pad_settings: (core/README.md "Pads").
 * Readings are 0..4095, after inverted:. */
#ifndef GM_PAD_CHANNEL_DEFAULT
#define GM_PAD_CHANNEL_DEFAULT 0
#endif
#ifndef GM_PAD_CURVE_DEFAULT
#define GM_PAD_CURVE_DEFAULT 0
#endif
#ifndef GM_PAD_AFTERTOUCH_DEFAULT
#define GM_PAD_AFTERTOUCH_DEFAULT 0
#endif
#ifndef GM_PAD_THRESHOLD_DEFAULT
#define GM_PAD_THRESHOLD_DEFAULT 200
#endif
#ifndef GM_PAD_RELEASE_DEFAULT
#define GM_PAD_RELEASE_DEFAULT 100
#endif
#ifndef GM_PAD_FULL_DEFAULT
#define GM_PAD_FULL_DEFAULT 3000
#endif
#ifndef GM_PAD_WINDOW_DEFAULT
#define GM_PAD_WINDOW_DEFAULT 3
#endif
#ifndef GM_PAD_RETRIGGER_DEFAULT
#define GM_PAD_RETRIGGER_DEFAULT 20
#endif

/* Presets in use on a fresh device: the YAML's list (sysex.md §4.5). */
#ifdef GM_N_YAML_PRESETS
#define GM_PRESETS_IN_USE GM_N_YAML_PRESETS
#else
#define GM_PRESETS_IN_USE 1
#endif

enum { G_MIDI, G_VELOCITY, G_BUTTONS, G_ANALOG, G_ENCODERS, G_DEBUG, G_PRESETS, G_PADS };
#define G_LAYERS (G_PADS + (GM_N_PADS > 0))
static const char *const groups[] = {
    "MIDI", "Velocity", "Buttons", "Analog", "Encoders", "Debug", "Presets",
#if GM_N_PADS > 0
    "Pads",                          /* only with pads: other schemas stay as they were */
#endif
#if GM_LAYER_PARAMS
    "Layers",                        /* only with layers or cycles */
#endif
};

#if (GM_HAS_MATRIX && GM_MATRIX_VELOCITY) || GM_N_PADS > 0
static const char *const curve_opts[] = { "Normal", "Soft", "Hard", "Fixed" };
#endif
static const char *const log_opts[] = { "Off", "Errors", "Warnings", "Info", "Debug" };
/* diag.reset_reason (GM_RESET_*), diag.settings (GM_SETTINGS_*): sysex.md 4.10 */
static const char *const reset_opts[] = {
    "Unknown", "Power-on", "Reset pin", "Software", "Watchdog", "Brown-out", "Crash", "Low-power",
};
static const char *const stored_opts[] = {
    "Loaded", "None stored", "Some refused", "Ignored (safe mode)", "Wiped by the reset combo",
};
#if GM_N_BUTTONS > 0
#ifndef GM_MCU
#define GM_MCU 0
#endif
/* The highest action this device has, + 1: the option list runs from 0 to
 * it, and an action it doesn't have is "" (params_set refuses it). */
#if GM_N_PADS > 0
#define N_ACTIONS (ACT_PAD_OCTAVE_UP + 1)
#elif GM_N_SYSEX
#define N_ACTIONS (ACT_SYSEX + 1)
#elif GM_MSG_MMC
#define N_ACTIONS (ACT_MMC + 1)
#elif GM_MSG_PANIC
#define N_ACTIONS (ACT_PANIC + 1)
#elif GM_MSG_PROGRAM
#define N_ACTIONS (ACT_PROGRAM + 1)
#else
#define N_ACTIONS (ACT_COUNT + (GM_MCU ? 1 : 0))
#endif
static const char *const action_opts[N_ACTIONS] = {
    "None", "CC momentary", "CC toggle", "CC ramp", "Note",
    "Octave down", "Octave up", "Transpose down", "Transpose up",
    "Pitch bend down", "Pitch bend up", "Program next", "Program previous",
    "Preset next", "Preset previous", "Preset select",
#if GM_MCU
    [ACT_MCU] = "Mackie Control",
#endif
#if GM_MSG_PROGRAM
    [ACT_PROGRAM] = "Program",
#endif
#if GM_MSG_PANIC
    [ACT_PANIC] = "Panic",
#endif
#if GM_MSG_MMC
    [ACT_MMC] = "MMC",
#endif
#if GM_N_SYSEX
    [ACT_SYSEX] = "SysEx",
#endif
#if GM_N_PADS > 0
    [ACT_PAD_OCTAVE_DOWN] = "Pad octave down",
    [ACT_PAD_OCTAVE_UP] = "Pad octave up",
#endif
};
#if GM_MSG_PROGRAM || GM_MSG_MMC || GM_N_SYSEX
#define BTN_NUMBER_LABEL "CC / note / number"
#else
#define BTN_NUMBER_LABEL "CC / note number"
#endif
#endif

/* analog.cc / encoder.cc: a CC, or an NRPN / RPN number, or a SysEx index */
#if GM_MSG_NRPN
#define NUM_MAX 16383
#else
#define NUM_MAX 127
#endif
#if GM_MSG_NRPN || GM_N_SYSEX
#define NUM_LABEL "CC / number"
#else
#define NUM_LABEL "CC"
#endif

/* Index labels are the components' display names (YAML name:, or from the
 * id): the builder's gm_*_names tables, in flash (gm_device.h). */

#if GM_N_ANALOGS > 0
static const char *const analog_mode_opts[GM_N_AN_MODES] = {
    "CC", "Pitch bend", "Channel pressure",
#if GM_MSG_CC14
    [AN_MODE_CC14] = "CC 14-bit",
#endif
#if GM_MSG_NRPN
    [AN_MODE_NRPN] = "NRPN", [AN_MODE_NRPN14] = "NRPN 14-bit",
    [AN_MODE_RPN] = "RPN", [AN_MODE_RPN14] = "RPN 14-bit",
#endif
#if GM_N_SYSEX
    [AN_MODE_SYSEX] = "SysEx",
#endif
};
#endif

#if GM_LAYER_PARAMS
/* the YAML's enabled: (gm_device_config.h; true unless a step says false) */
static const int32_t mode_enabled_defs[GM_N_MODES] = { GM_MODES_ENABLED };
#endif

#if GM_N_ENCODERS > 0
#if GM_ENC_MODES
#define enc_mode_defs gm_encoder_mode_defaults   /* the YAML's mode: */
#else
static const int32_t enc_mode_defs[GM_N_ENCODERS] = { 0 };
#endif
static const char *const enc_mode_opts[GM_N_ENC_MODES] = {
    "Absolute", "Relative (two's complement)", "Relative (offset 64)", "Relative (sign-magnitude)",
#if GM_MSG_CC14
    [ENC_CC14] = "Absolute CC 14-bit",
#endif
#if GM_MSG_NRPN
    [ENC_NRPN] = "Absolute NRPN", [ENC_NRPN14] = "Absolute NRPN 14-bit",
    [ENC_RPN] = "Absolute RPN", [ENC_RPN14] = "Absolute RPN 14-bit",
#endif
};
#endif

#ifdef GM_CONFIG_HASH
/* The resolved YAML's hash (core/README.md "Config hash"): the builder's, so
 * an editor can tell which device file this firmware was built from. */
static int32_t config_hash = GM_CONFIG_HASH;
#endif

static const param_def_t params[] = {
    { .id = P_CHANNEL, .flags = PF_PRESET, .type = PT_INT, .group = G_MIDI, .count = 1,
      .min = 1, .max = 16, .def = 1,
      .name = "midi.channel", .label = "MIDI channel", .values = &gm_cfg.channel },
    { .id = P_TRANSPOSE, .flags = PF_PRESET, .type = PT_INT, .group = G_MIDI, .count = 1,
      .min = -24, .max = 24, .def = 0, .unit = "semitones",
      .name = "midi.transpose", .label = "Transpose", .values = &gm_cfg.transpose },
    { .id = P_OCTAVE, .flags = PF_PRESET, .type = PT_INT, .group = G_MIDI, .count = 1,
      .min = -4, .max = 4, .def = 0,
      .name = "midi.octave", .label = "Octave", .values = &gm_cfg.octave },
#if GM_DIN
    { .id = P_DIN_THRU, .type = PT_BOOL, .group = G_MIDI, .count = 1,
      .min = 0, .max = 1, .def = GM_DIN_THRU_DEFAULT,
      .name = "midi.thru", .label = "DIN In to DIN Out (thru)", .values = &gm_cfg.din_thru },
    { .id = P_DIN_OUT, .type = PT_BOOL, .group = G_MIDI, .count = 1,
      .min = 0, .max = 1, .def = GM_DIN_OUT_DEFAULT,
      .name = "midi.din_out", .label = "Controller to DIN Out", .values = &gm_cfg.din_out },
#if GM_DIN_MODE == 2                 /* usb_merge */
    { .id = P_DIN_USB, .type = PT_BOOL, .group = G_MIDI, .count = 1,
      .min = 0, .max = 1, .def = GM_DIN_USB_DEFAULT,
      .name = "midi.usb_to_din", .label = "USB to DIN Out", .values = &gm_cfg.din_usb },
#endif
#endif

#if GM_HAS_MATRIX && GM_MATRIX_VELOCITY
    { .id = P_VEL_CURVE, .flags = PF_PRESET, .type = PT_ENUM, .group = G_VELOCITY, .count = 1,
      .min = 0, .max = 3, .def = CURVE_NORMAL, .options = curve_opts, .option_count = 4,
      .name = "velocity.curve", .label = "Velocity curve", .values = &gm_cfg.vel_curve },
#endif
    { .id = P_VEL_FIXED, .flags = PF_PRESET, .type = PT_INT, .group = G_VELOCITY, .count = 1,
      .min = 1, .max = 127, .def = 100,
      .name = "velocity.fixed", .label = "Fixed velocity", .values = &gm_cfg.vel_fixed },
#if GM_HAS_MATRIX && GM_MATRIX_VELOCITY
    { .id = P_VEL_FAST, .type = PT_INT, .group = G_VELOCITY, .count = 1,
      .min = 1, .max = 500, .def = GM_VEL_FAST_DEFAULT, .unit = "0.1 ms",
      .name = "velocity.fast", .label = "Key time for velocity 127", .values = &gm_cfg.vel_fast },
    { .id = P_VEL_SLOW, .type = PT_INT, .group = G_VELOCITY, .count = 1,
      .min = 20, .max = 2000, .def = GM_VEL_SLOW_DEFAULT, .unit = "0.1 ms",
      .name = "velocity.slow", .label = "Key time for velocity 1", .values = &gm_cfg.vel_slow },
#endif

#if GM_N_OWN_BUTTONS > 0
    { .id = P_BTN_ACTION, .flags = PF_PRESET, .type = PT_ENUM, .group = G_BUTTONS, .count = GM_N_OWN_BUTTONS,
      .min = 0, .max = N_ACTIONS - 1, .defs = gm_button_action_defaults,
      .options = action_opts, .option_count = N_ACTIONS, .index_labels = gm_button_names,
      .name = "button.action", .label = "Action", .values = gm_cfg.btn_action },
    { .id = P_BTN_NUMBER, .flags = PF_PRESET, .type = PT_INT, .group = G_BUTTONS, .count = GM_N_OWN_BUTTONS,
      .min = 0, .max = 127, .defs = gm_button_number_defaults, .index_labels = gm_button_names,
      .name = "button.number", .label = BTN_NUMBER_LABEL, .values = gm_cfg.btn_number },
#endif
#if GM_N_BUTTONS > 0
    { .id = P_BEND_GLIDE, .flags = PF_PRESET, .type = PT_INT, .group = G_BUTTONS, .count = 1,
      .min = 0, .max = 2000, .def = 80, .unit = "ms",
      .name = "bend.glide", .label = "Pitch bend glide", .values = &gm_cfg.bend_glide },
    { .id = P_RAMP_TIME, .flags = PF_PRESET, .type = PT_INT, .group = G_BUTTONS, .count = 1,
      .min = 0, .max = 5000, .def = 300, .unit = "ms",
      .name = "ramp.time", .label = "CC ramp time", .values = &gm_cfg.ramp_time },
#endif

#if GM_N_ANALOGS > 0
    { .id = P_ANALOG_CC, .flags = PF_PRESET, .type = PT_INT, .group = G_ANALOG, .count = GM_N_ANALOGS,
      .min = 0, .max = NUM_MAX, .defs = gm_analog_cc_defaults, .index_labels = gm_analog_names,
      .name = "analog.cc", .label = NUM_LABEL, .values = gm_cfg.analog_cc },
    { .id = P_ANALOG_MODE, .flags = PF_PRESET, .type = PT_ENUM, .group = G_ANALOG, .count = GM_N_ANALOGS,
      .min = 0, .max = GM_N_AN_MODES - 1, .defs = gm_analog_mode_defaults, .options = analog_mode_opts,
      .option_count = GM_N_AN_MODES,
      .index_labels = gm_analog_names,
      .name = "analog.mode", .label = "Mode", .values = gm_cfg.analog_mode },
#endif

#if GM_N_OWN_ENCODERS > 0
    { .id = P_ENC_CC, .flags = PF_PRESET, .type = PT_INT, .group = G_ENCODERS, .count = GM_N_OWN_ENCODERS,
      .min = 0, .max = NUM_MAX, .defs = gm_encoder_cc_defaults, .index_labels = gm_encoder_names,
      .name = "encoder.cc", .label = NUM_LABEL, .values = gm_cfg.enc_cc },
    { .id = P_ENC_MODE, .flags = PF_PRESET, .type = PT_ENUM, .group = G_ENCODERS, .count = GM_N_OWN_ENCODERS,
      .min = 0, .max = GM_N_ENC_MODES - 1, .defs = enc_mode_defs, .options = enc_mode_opts,
      .option_count = GM_N_ENC_MODES,
      .index_labels = gm_encoder_names,
      .name = "encoder.mode", .label = "Mode", .values = gm_cfg.enc_mode },
#endif

    { .id = P_EVENTS, .type = PT_BOOL, .group = G_DEBUG, .count = 1, .flags = PF_VOLATILE,
      .min = 0, .max = 1, .def = 0,
      .name = "debug.events", .label = "Send EVENT messages", .values = &gm_cfg.events },
    { .id = P_LOG_LEVEL, .type = PT_ENUM, .group = G_DEBUG, .count = 1,
      .min = 0, .max = 4, .def = 3, .options = log_opts, .option_count = 5,
      .name = "debug.log_level", .label = "Log level", .values = &gm_cfg.log_level },
    /* diagnostics: what the core and the platform found (gm_safety.h) */
    { .id = P_DIAG_RESET, .type = PT_ENUM, .group = G_DEBUG, .count = 1, .flags = PF_READ_ONLY | PF_VOLATILE,
      .min = 0, .max = 7, .def = 0, .options = reset_opts, .option_count = 8,
      .name = "diag.reset_reason", .label = "Last reset", .values = &gm_diag_param[0] },
    { .id = P_DIAG_BOOT_LOOP, .type = PT_BOOL, .group = G_DEBUG, .count = 1, .flags = PF_READ_ONLY | PF_VOLATILE,
      .min = 0, .max = 1, .def = 0,
      .name = "diag.boot_loop", .label = "Started after the boot-loop fallback", .values = &gm_diag_param[1] },
    { .id = P_DIAG_SETTINGS, .type = PT_ENUM, .group = G_DEBUG, .count = 1, .flags = PF_READ_ONLY | PF_VOLATILE,
      .min = 0, .max = 4, .def = 0, .options = stored_opts, .option_count = 5,
      .name = "diag.settings", .label = "Stored settings at power-up", .values = &gm_diag_param[2] },
    { .id = P_DIAG_SCAN_MAX, .type = PT_INT, .group = G_DEBUG, .count = 1, .flags = PF_READ_ONLY | PF_VOLATILE,
      .min = 0, .max = 0x7FFFFFFF, .def = 0, .unit = "us",
      .name = "diag.scan_max", .label = "Longest scan", .values = &gm_diag_param[3] },
    { .id = P_DIAG_LOOP_MAX, .type = PT_INT, .group = G_DEBUG, .count = 1, .flags = PF_READ_ONLY | PF_VOLATILE,
      .min = 0, .max = 0x7FFFFFFF, .def = 0, .unit = "us",
      .name = "diag.loop_max", .label = "Longest main loop pass", .values = &gm_diag_param[4] },
    { .id = P_DIAG_SCAN_PERIOD, .type = PT_INT, .group = G_DEBUG, .count = 1, .flags = PF_READ_ONLY | PF_VOLATILE,
      .min = 0, .max = 0x7FFFFFFF, .def = 0, .unit = "us",
      .name = "diag.scan_period", .label = "Scan period", .values = &gm_diag_param[5] },
    { .id = P_DIAG_UPTIME, .type = PT_INT, .group = G_DEBUG, .count = 1, .flags = PF_READ_ONLY | PF_VOLATILE,
      .min = 0, .max = 0x7FFFFFFF, .def = 0, .unit = "s",
      .name = "diag.uptime", .label = "Uptime", .values = &gm_diag_param[6] },
#ifdef GM_CONFIG_HASH
    { .id = P_CONFIG_HASH, .flags = PF_READ_ONLY | PF_VOLATILE, .type = PT_INT, .group = G_DEBUG, .count = 1,
      .min = 0, .max = 0x7FFFFFFF, .def = GM_CONFIG_HASH,
      .name = "config.hash", .label = "Config hash", .values = &config_hash },
#endif

    { .id = P_PRESET_ACTIVE, .type = PT_INT, .group = G_PRESETS, .count = 1,
      .min = 1, .max = GM_N_PRESETS, .def = 1,
      .name = "preset.active", .label = "Active preset", .values = &gm_cfg.preset_active },
#if GM_N_PRESETS > 1
    { .id = P_PRESET_PROGRAM, .flags = PF_PRESET, .type = PT_INT, .group = G_PRESETS, .count = 1,
      .min = -1, .max = 127, .def = -1,
      .name = "preset.program", .label = "Program Change on select (-1 = none)",
      .values = &gm_cfg.preset_program },
#if GM_PRESET_BANK
    { .id = P_PRESET_BANK, .flags = PF_PRESET, .type = PT_INT, .group = G_PRESETS, .count = 1,
      .min = -1, .max = 16383, .def = -1,
      .name = "preset.bank", .label = "Bank Select before it (-1 = none)", .values = &gm_cfg.preset_bank },
#endif
    { .id = P_PRESET_DIRTY, .flags = PF_VOLATILE | PF_READ_ONLY, .type = PT_BOOL,
      .group = G_PRESETS, .count = 1, .min = 0, .max = 1, .def = 0,
      .name = "preset.dirty", .label = "Unsaved changes", .values = &gm_cfg.preset_dirty },
    /* Read-only to a host (PRESET_SAVE, PRESET_NAME and PRESET_DELETE change
     * it), but stored with the globals (presets.c storable()). */
    { .id = P_PRESET_COUNT, .flags = PF_READ_ONLY, .type = PT_INT, .group = G_PRESETS, .count = 1,
      .min = 1, .max = GM_N_PRESETS, .def = GM_PRESETS_IN_USE,
      .name = "preset.count", .label = "Presets in use", .values = &gm_cfg.preset_count },
#endif

#if GM_LAYER_PARAMS
    /* layers: global, stored once with the other globals. The twins' values
     * are the tails of the button / encoder arrays. */
    { .id = P_LAYER_ENABLED, .type = PT_BOOL, .group = G_LAYERS, .count = GM_N_MODES,
      .min = 0, .max = 1, .defs = mode_enabled_defs, .index_labels = gm_mode_names,
      .name = "layer.enabled", .label = "Mode enabled", .values = gm_cfg.layer_enabled },
#if GM_BTN_TWINS > 0
    { .id = P_LAYER_BTN_ACTION, .type = PT_ENUM, .group = G_LAYERS, .count = GM_BTN_TWINS,
      .min = 0, .max = N_ACTIONS - 1, .defs = gm_button_action_defaults + GM_N_OWN_BUTTONS,
      .options = action_opts, .option_count = N_ACTIONS, .index_labels = gm_button_names + GM_N_OWN_BUTTONS,
      .name = "layer.button.action", .label = "Twin action", .values = gm_cfg.btn_action + GM_N_OWN_BUTTONS },
    { .id = P_LAYER_BTN_NUMBER, .type = PT_INT, .group = G_LAYERS, .count = GM_BTN_TWINS,
      .min = 0, .max = 127, .defs = gm_button_number_defaults + GM_N_OWN_BUTTONS,
      .index_labels = gm_button_names + GM_N_OWN_BUTTONS,
      .name = "layer.button.number", .label = "Twin CC / note number",
      .values = gm_cfg.btn_number + GM_N_OWN_BUTTONS },
#endif
#if GM_ENC_TWINS > 0
    { .id = P_LAYER_ENC_CC, .type = PT_INT, .group = G_LAYERS, .count = GM_ENC_TWINS,
      .min = -1, .max = NUM_MAX, .defs = gm_encoder_cc_defaults + GM_N_OWN_ENCODERS,
      .index_labels = gm_encoder_names + GM_N_OWN_ENCODERS,
      .name = "layer.encoder.cc", .label = "Twin CC (-1 = none: EVENTs only)",
      .values = gm_cfg.enc_cc + GM_N_OWN_ENCODERS },
    { .id = P_LAYER_ENC_MODE, .type = PT_ENUM, .group = G_LAYERS, .count = GM_ENC_TWINS,
      .min = 0, .max = GM_N_ENC_MODES - 1, .defs = enc_mode_defs + GM_N_OWN_ENCODERS, .options = enc_mode_opts,
      .option_count = GM_N_ENC_MODES,
      .index_labels = gm_encoder_names + GM_N_OWN_ENCODERS,
      .name = "layer.encoder.mode", .label = "Twin mode", .values = gm_cfg.enc_mode + GM_N_OWN_ENCODERS },
#endif
#endif

#if GM_N_PADS > 0
    /* the mapping: per preset */
    { .id = P_PAD_NOTE, .flags = PF_PRESET, .type = PT_NOTE, .group = G_PADS, .count = GM_N_PADS,
      .min = 0, .max = 127, .defs = gm_pad_note_defaults, .index_labels = gm_pad_names,
      .name = "pad.note", .label = "Note", .values = gm_cfg.pad_note },
    { .id = P_PAD_CHANNEL, .flags = PF_PRESET, .type = PT_INT, .group = G_PADS, .count = 1,
      .min = 0, .max = 16, .def = GM_PAD_CHANNEL_DEFAULT,
      .name = "pad.channel", .label = "Channel (0 = MIDI channel)", .values = &gm_cfg.pad_channel },
    { .id = P_PAD_CURVE, .flags = PF_PRESET, .type = PT_ENUM, .group = G_PADS, .count = 1,
      .min = 0, .max = 3, .def = GM_PAD_CURVE_DEFAULT, .options = curve_opts, .option_count = 4,
      .name = "pad.curve", .label = "Velocity curve", .values = &gm_cfg.pad_curve },
    { .id = P_PAD_AFTERTOUCH, .flags = PF_PRESET, .type = PT_BOOL, .group = G_PADS, .count = 1,
      .min = 0, .max = 1, .def = GM_PAD_AFTERTOUCH_DEFAULT,
      .name = "pad.aftertouch", .label = "Poly aftertouch", .values = &gm_cfg.pad_aftertouch },
    { .id = P_PAD_OCTAVE, .flags = PF_PRESET, .type = PT_INT, .group = G_PADS, .count = 1,
      .min = -4, .max = 4, .def = 0,
      .name = "pad.octave", .label = "Octave", .values = &gm_cfg.pad_octave },
    /* the sensing: global (the pads' calibration) */
    { .id = P_PAD_THRESHOLD, .type = PT_INT, .group = G_PADS, .count = 1,
      .min = 1, .max = 4095, .def = GM_PAD_THRESHOLD_DEFAULT,
      .name = "pad.threshold", .label = "Hit threshold (reading)", .values = &gm_cfg.pad_threshold },
    { .id = P_PAD_RELEASE, .type = PT_INT, .group = G_PADS, .count = 1,
      .min = 0, .max = 4094, .def = GM_PAD_RELEASE_DEFAULT,
      .name = "pad.release", .label = "Released below (reading)", .values = &gm_cfg.pad_release },
    { .id = P_PAD_FULL, .type = PT_INT, .group = G_PADS, .count = 1,
      .min = 2, .max = 4095, .def = GM_PAD_FULL_DEFAULT,
      .name = "pad.full", .label = "Peak for velocity 127 (reading)", .values = &gm_cfg.pad_full },
    { .id = P_PAD_WINDOW, .type = PT_INT, .group = G_PADS, .count = 1,
      .min = 1, .max = 50, .def = GM_PAD_WINDOW_DEFAULT, .unit = "ms",
      .name = "pad.window", .label = "Peak window", .values = &gm_cfg.pad_window },
    { .id = P_PAD_RETRIGGER, .type = PT_INT, .group = G_PADS, .count = 1,
      .min = 0, .max = 500, .def = GM_PAD_RETRIGGER_DEFAULT, .unit = "ms",
      .name = "pad.retrigger", .label = "Retrigger guard", .values = &gm_cfg.pad_retrigger },
#endif
};

const param_table_t app_params = {
    .groups = groups,
    .group_count = sizeof(groups) / sizeof(groups[0]),
    .param_count = sizeof(params) / sizeof(params[0]),
    .params = params,
};
