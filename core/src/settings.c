/*
 * settings.c: the controller's param table (the schema an editor reads,
 * protocol/sysex.md 4.4).
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

enum { G_MIDI, G_VELOCITY, G_BUTTONS, G_ANALOG, G_ENCODERS, G_DEBUG };
static const char *const groups[] = {
    "MIDI", "Velocity", "Buttons", "Analog", "Encoders", "Debug",
};

#if GM_HAS_MATRIX && GM_MATRIX_VELOCITY
static const char *const curve_opts[] = { "Normal", "Soft", "Hard", "Fixed" };
#endif
static const char *const log_opts[] = { "Off", "Errors", "Warnings", "Info", "Debug" };
#if GM_N_BUTTONS > 0
static const char *const action_opts[ACT_COUNT] = {
    "None", "CC momentary", "CC toggle", "CC ramp", "Note",
    "Octave down", "Octave up", "Transpose down", "Transpose up",
    "Pitch bend down", "Pitch bend up", "Program next", "Program previous",
};
#endif

/* Index labels come from the device's component labels. */
static const char *button_labels[GM_ARR(GM_N_BUTTONS)];
static const char *analog_labels[GM_ARR(GM_N_ANALOGS)];
static const char *encoder_labels[GM_ARR(GM_N_ENCODERS)];

#if GM_N_ENCODERS > 0
static const int32_t enc_mode_defs[GM_N_ENCODERS] = { 0 };
static const char *const enc_mode_opts[] = {
    "Absolute", "Relative (two's complement)", "Relative (offset 64)",
};
#endif

void gm_settings_init(void)
{
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        button_labels[i] = gm_device.buttons[i].label;
    for (uint32_t i = 0; i < gm_device.n_analogs; i++)
        analog_labels[i] = gm_device.analogs[i].label;
    for (uint32_t i = 0; i < gm_device.n_encoders; i++)
        encoder_labels[i] = gm_device.encoders[i].label;
}

static const param_def_t params[] = {
    { .id = P_CHANNEL, .type = PT_INT, .group = G_MIDI, .count = 1,
      .min = 1, .max = 16, .def = 1,
      .name = "midi.channel", .label = "MIDI channel", .values = &gm_cfg.channel },
    { .id = P_TRANSPOSE, .type = PT_INT, .group = G_MIDI, .count = 1,
      .min = -24, .max = 24, .def = 0, .unit = "semitones",
      .name = "midi.transpose", .label = "Transpose", .values = &gm_cfg.transpose },
    { .id = P_OCTAVE, .type = PT_INT, .group = G_MIDI, .count = 1,
      .min = -4, .max = 4, .def = 0,
      .name = "midi.octave", .label = "Octave", .values = &gm_cfg.octave },

#if GM_HAS_MATRIX && GM_MATRIX_VELOCITY
    { .id = P_VEL_CURVE, .type = PT_ENUM, .group = G_VELOCITY, .count = 1,
      .min = 0, .max = 3, .def = CURVE_NORMAL, .options = curve_opts, .option_count = 4,
      .name = "velocity.curve", .label = "Velocity curve", .values = &gm_cfg.vel_curve },
#endif
    { .id = P_VEL_FIXED, .type = PT_INT, .group = G_VELOCITY, .count = 1,
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
    { .id = P_BTN_ACTION, .type = PT_ENUM, .group = G_BUTTONS, .count = GM_N_BUTTONS,
      .min = 0, .max = ACT_COUNT - 1, .defs = gm_button_action_defaults,
      .options = action_opts, .option_count = ACT_COUNT, .index_labels = button_labels,
      .name = "button.action", .label = "Action", .values = gm_cfg.btn_action },
    { .id = P_BTN_NUMBER, .type = PT_INT, .group = G_BUTTONS, .count = GM_N_BUTTONS,
      .min = 0, .max = 127, .defs = gm_button_number_defaults, .index_labels = button_labels,
      .name = "button.number", .label = "CC / note number", .values = gm_cfg.btn_number },
    { .id = P_BEND_GLIDE, .type = PT_INT, .group = G_BUTTONS, .count = 1,
      .min = 0, .max = 2000, .def = 80, .unit = "ms",
      .name = "bend.glide", .label = "Pitch bend glide", .values = &gm_cfg.bend_glide },
    { .id = P_RAMP_TIME, .type = PT_INT, .group = G_BUTTONS, .count = 1,
      .min = 0, .max = 5000, .def = 300, .unit = "ms",
      .name = "ramp.time", .label = "CC ramp time", .values = &gm_cfg.ramp_time },
#endif

#if GM_N_ANALOGS > 0
    { .id = P_ANALOG_CC, .type = PT_INT, .group = G_ANALOG, .count = GM_N_ANALOGS,
      .min = 0, .max = 127, .defs = gm_analog_cc_defaults, .index_labels = analog_labels,
      .name = "analog.cc", .label = "CC", .values = gm_cfg.analog_cc },
#endif

#if GM_N_ENCODERS > 0
    { .id = P_ENC_CC, .type = PT_INT, .group = G_ENCODERS, .count = GM_N_ENCODERS,
      .min = 0, .max = 127, .defs = gm_encoder_cc_defaults, .index_labels = encoder_labels,
      .name = "encoder.cc", .label = "CC", .values = gm_cfg.enc_cc },
    { .id = P_ENC_MODE, .type = PT_ENUM, .group = G_ENCODERS, .count = GM_N_ENCODERS,
      .min = 0, .max = 2, .defs = enc_mode_defs, .options = enc_mode_opts, .option_count = 3,
      .index_labels = encoder_labels,
      .name = "encoder.mode", .label = "Mode", .values = gm_cfg.enc_mode },
#endif

    { .id = P_EVENTS, .type = PT_BOOL, .group = G_DEBUG, .count = 1,
      .min = 0, .max = 1, .def = 0,
      .name = "debug.events", .label = "Send EVENT messages", .values = &gm_cfg.events },
    { .id = P_LOG_LEVEL, .type = PT_ENUM, .group = G_DEBUG, .count = 1,
      .min = 0, .max = 4, .def = 3, .options = log_opts, .option_count = 5,
      .name = "debug.log_level", .label = "Log level", .values = &gm_cfg.log_level },
};

const param_table_t app_params = {
    .groups = groups,
    .group_count = sizeof(groups) / sizeof(groups[0]),
    .param_count = sizeof(params) / sizeof(params[0]),
    .params = params,
};
