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

gm_settings_t gm_cfg;

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

enum { G_MIDI, G_VELOCITY, G_BUTTONS, G_ANALOG, G_ENCODERS, G_DEBUG, G_PRESETS, G_PADS };
static const char *const groups[] = {
    "MIDI", "Velocity", "Buttons", "Analog", "Encoders", "Debug", "Presets",
#if GM_N_PADS > 0
    "Pads",                          /* only with pads: other schemas stay as they were */
#endif
};

#if (GM_HAS_MATRIX && GM_MATRIX_VELOCITY) || GM_N_PADS > 0
static const char *const curve_opts[] = { "Normal", "Soft", "Hard", "Fixed" };
#endif
static const char *const log_opts[] = { "Off", "Errors", "Warnings", "Info", "Debug" };
#if GM_N_BUTTONS > 0
#ifndef GM_MCU
#define GM_MCU 0
#endif
#define N_ACTIONS (ACT_COUNT + (GM_MCU ? 1 : 0))
static const char *const action_opts[N_ACTIONS] = {
    "None", "CC momentary", "CC toggle", "CC ramp", "Note",
    "Octave down", "Octave up", "Transpose down", "Transpose up",
    "Pitch bend down", "Pitch bend up", "Program next", "Program previous",
    "Preset next", "Preset previous", "Preset select",
#if GM_MCU
    "Mackie Control",
#endif
};
#endif

/* Index labels are the components' display names (YAML name:, or from the id). */
static const char *button_labels[GM_ARR(GM_N_BUTTONS)];
static const char *analog_labels[GM_ARR(GM_N_ANALOGS)];
static const char *encoder_labels[GM_ARR(GM_N_ENCODERS)];
#if GM_N_PADS > 0
static const char *pad_labels[GM_N_PADS];
#endif

#if GM_N_ANALOGS > 0
static const char *const analog_mode_opts[] = { "CC", "Pitch bend" };
#endif

#if GM_N_ENCODERS > 0
static const int32_t enc_mode_defs[GM_N_ENCODERS] = { 0 };
static const char *const enc_mode_opts[] = {
    "Absolute", "Relative (two's complement)", "Relative (offset 64)",
};
#endif

void gm_settings_init(void)
{
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        button_labels[i] = gm_device.buttons[i].name;
    for (uint32_t i = 0; i < gm_device.n_analogs; i++)
        analog_labels[i] = gm_device.analogs[i].name;
    for (uint32_t i = 0; i < gm_device.n_encoders; i++)
        encoder_labels[i] = gm_device.encoders[i].name;
#if GM_N_PADS > 0
    for (uint32_t i = 0; i < GM_N_PADS; i++)
        pad_labels[i] = gm_pads[i].name;
#endif
}

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

#if GM_N_BUTTONS > 0
    { .id = P_BTN_ACTION, .flags = PF_PRESET, .type = PT_ENUM, .group = G_BUTTONS, .count = GM_N_BUTTONS,
      .min = 0, .max = N_ACTIONS - 1, .defs = gm_button_action_defaults,
      .options = action_opts, .option_count = N_ACTIONS, .index_labels = button_labels,
      .name = "button.action", .label = "Action", .values = gm_cfg.btn_action },
    { .id = P_BTN_NUMBER, .flags = PF_PRESET, .type = PT_INT, .group = G_BUTTONS, .count = GM_N_BUTTONS,
      .min = 0, .max = 127, .defs = gm_button_number_defaults, .index_labels = button_labels,
      .name = "button.number", .label = "CC / note number", .values = gm_cfg.btn_number },
    { .id = P_BEND_GLIDE, .flags = PF_PRESET, .type = PT_INT, .group = G_BUTTONS, .count = 1,
      .min = 0, .max = 2000, .def = 80, .unit = "ms",
      .name = "bend.glide", .label = "Pitch bend glide", .values = &gm_cfg.bend_glide },
    { .id = P_RAMP_TIME, .flags = PF_PRESET, .type = PT_INT, .group = G_BUTTONS, .count = 1,
      .min = 0, .max = 5000, .def = 300, .unit = "ms",
      .name = "ramp.time", .label = "CC ramp time", .values = &gm_cfg.ramp_time },
#endif

#if GM_N_ANALOGS > 0
    { .id = P_ANALOG_CC, .flags = PF_PRESET, .type = PT_INT, .group = G_ANALOG, .count = GM_N_ANALOGS,
      .min = 0, .max = 127, .defs = gm_analog_cc_defaults, .index_labels = analog_labels,
      .name = "analog.cc", .label = "CC", .values = gm_cfg.analog_cc },
    { .id = P_ANALOG_MODE, .flags = PF_PRESET, .type = PT_ENUM, .group = G_ANALOG, .count = GM_N_ANALOGS,
      .min = 0, .max = 1, .defs = gm_analog_mode_defaults, .options = analog_mode_opts, .option_count = 2,
      .index_labels = analog_labels,
      .name = "analog.mode", .label = "Mode", .values = gm_cfg.analog_mode },
#endif

#if GM_N_ENCODERS > 0
    { .id = P_ENC_CC, .flags = PF_PRESET, .type = PT_INT, .group = G_ENCODERS, .count = GM_N_ENCODERS,
      .min = 0, .max = 127, .defs = gm_encoder_cc_defaults, .index_labels = encoder_labels,
      .name = "encoder.cc", .label = "CC", .values = gm_cfg.enc_cc },
    { .id = P_ENC_MODE, .flags = PF_PRESET, .type = PT_ENUM, .group = G_ENCODERS, .count = GM_N_ENCODERS,
      .min = 0, .max = 2, .defs = enc_mode_defs, .options = enc_mode_opts, .option_count = 3,
      .index_labels = encoder_labels,
      .name = "encoder.mode", .label = "Mode", .values = gm_cfg.enc_mode },
#endif

    { .id = P_EVENTS, .type = PT_BOOL, .group = G_DEBUG, .count = 1, .flags = PF_VOLATILE,
      .min = 0, .max = 1, .def = 0,
      .name = "debug.events", .label = "Send EVENT messages", .values = &gm_cfg.events },
    { .id = P_LOG_LEVEL, .type = PT_ENUM, .group = G_DEBUG, .count = 1,
      .min = 0, .max = 4, .def = 3, .options = log_opts, .option_count = 5,
      .name = "debug.log_level", .label = "Log level", .values = &gm_cfg.log_level },

    { .id = P_PRESET_ACTIVE, .type = PT_INT, .group = G_PRESETS, .count = 1,
      .min = 1, .max = GM_N_PRESETS, .def = 1,
      .name = "preset.active", .label = "Active preset", .values = &gm_cfg.preset_active },
#if GM_N_PRESETS > 1
    { .id = P_PRESET_PROGRAM, .flags = PF_PRESET, .type = PT_INT, .group = G_PRESETS, .count = 1,
      .min = -1, .max = 127, .def = -1,
      .name = "preset.program", .label = "Program Change on select (-1 = none)",
      .values = &gm_cfg.preset_program },
    { .id = P_PRESET_DIRTY, .flags = PF_VOLATILE | PF_READ_ONLY, .type = PT_BOOL,
      .group = G_PRESETS, .count = 1, .min = 0, .max = 1, .def = 0,
      .name = "preset.dirty", .label = "Unsaved changes", .values = &gm_cfg.preset_dirty },
#endif

#if GM_N_PADS > 0
    /* the mapping: per preset */
    { .id = P_PAD_NOTE, .flags = PF_PRESET, .type = PT_NOTE, .group = G_PADS, .count = GM_N_PADS,
      .min = 0, .max = 127, .defs = gm_pad_note_defaults, .index_labels = pad_labels,
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
