/* gm_device_config: do not edit; generated from /tmp/claude-1000/-home-vantreeseba-code-cubicecho-gemifi/042868f3-3a64-4313-a18f-c7aaef12a119/scratchpad/code61-nopads.yaml
 * by tools/builder/gemifi_build.py (format: core/README.md). */

#include "gm_device.h"
#include "gm_device_config.h"

static const gm_pin_t hub7_rows[4] = {
    { 6, 0, 16 }, /* rows.16 */
    { 6, 0, 17 }, /* rows.17 */
    { 6, 0, 18 }, /* rows.18 */
    { 6, 0, 19 }, /* rows.19 */
};
static const gm_pin_t hub7_cols[8] = {
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 24 }, /* PB8 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 25 }, /* PB9 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 26 }, /* PB10 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 27 }, /* PB11 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 28 }, /* PB12 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 29 }, /* PB13 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 30 }, /* PB14 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 31 }, /* PB15 */
};
static const gm_pin_t hub8_rows[1] = {
    { 6, 0, 20 }, /* rows.20 */
};
static const gm_pin_t hub8_cols[8] = {
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 24 }, /* PB8 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 25 }, /* PB9 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 26 }, /* PB10 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 27 }, /* PB11 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 28 }, /* PB12 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 29 }, /* PB13 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 30 }, /* PB14 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 31 }, /* PB15 */
};
static const gm_pin_t hub9_rows[2] = {
    { 6, 0, 21 }, /* rows.21 */
    { 6, 0, 22 }, /* rows.22 */
};
static const gm_pin_t hub9_cols[8] = {
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 24 }, /* PB8 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 25 }, /* PB9 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 26 }, /* PB10 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 27 }, /* PB11 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 28 }, /* PB12 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 29 }, /* PB13 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 30 }, /* PB14 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 31 }, /* PB15 */
};
static const gm_hub_t hubs[GM_N_HUBS] = {
    { /* 1: fader_mux (cd74hc4051) */
        .type = GM_HUB_MUX, .chain = 0, .n_sel = 3, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { { 0, 0, 32 }, { 0, 0, 33 }, { 0, 0, 34 }, GM_NO_PIN },
        .common = { 0, 0, 7 }, .enable = GM_NO_PIN,
        .settle_us = 10,
    },
    { /* 2: pad_mux_a (cd74hc4051) */
        .type = GM_HUB_MUX, .chain = 0, .n_sel = 3, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { { 0, 0, 32 }, { 0, 0, 33 }, { 0, 0, 34 }, GM_NO_PIN },
        .common = { 0, 0, 0 }, .enable = GM_NO_PIN,
        .settle_us = 10,
    },
    { /* 3: pad_mux_b (cd74hc4051) */
        .type = GM_HUB_MUX, .chain = 0, .n_sel = 3, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { { 0, 0, 32 }, { 0, 0, 33 }, { 0, 0, 34 }, GM_NO_PIN },
        .common = { 0, 0, 1 }, .enable = GM_NO_PIN,
        .settle_us = 10,
    },
    { /* 4: leds_a (tm1628) */
        .type = GM_HUB_TM1628, .chain = 0, .n_sel = 0, .i2c_addr = 0x00,
        .data = { 0, GM_PIN_OPEN_DRAIN, 43 }, .clock = { 0, 0, 42 }, .latch = { 0, 0, 15 },
        .sel = { GM_NO_PIN, GM_NO_PIN, GM_NO_PIN, GM_NO_PIN },
        .common = GM_NO_PIN, .enable = GM_NO_PIN,
        .settle_us = 0,
        .ram_bytes = 14, .brightness = 7, .grids = 0,
    },
    { /* 5: leds_b (tm1628) */
        .type = GM_HUB_TM1628, .chain = 0, .n_sel = 0, .i2c_addr = 0x00,
        .data = { 0, GM_PIN_OPEN_DRAIN, 43 }, .clock = { 0, 0, 42 }, .latch = { 0, 0, 44 },
        .sel = { GM_NO_PIN, GM_NO_PIN, GM_NO_PIN, GM_NO_PIN },
        .common = GM_NO_PIN, .enable = GM_NO_PIN,
        .settle_us = 0,
        .ram_bytes = 6, .brightness = 7, .grids = 0,
    },
    { /* 6: rows (decoder) */
        .type = GM_HUB_DECODER, .chain = 0, .n_sel = 3, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { { 0, 0, 19 }, { 0, 0, 20 }, { 0, 0, 21 }, GM_NO_PIN },
        .common = GM_NO_PIN, .enable = GM_NO_PIN,
        .settle_us = 0,
        .en = { { 0, 0, 22 }, { 0, 0, 23 }, GM_NO_PIN }, .n_en = 2,
        .n_groups = 3, .group_lv = { 1, 2, 0, 0 },
    },
    { /* 7: panel (switch_matrix) */
        .type = GM_HUB_SWMATRIX, .chain = 0, .n_sel = 0, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { GM_NO_PIN, GM_NO_PIN, GM_NO_PIN, GM_NO_PIN },
        .common = GM_NO_PIN, .enable = GM_NO_PIN,
        .settle_us = 4,
        .rows = hub7_rows, .cols = hub7_cols, .n_rows = 4, .n_cols = 8,
    },
    { /* 8: panel_row20 (switch_matrix) */
        .type = GM_HUB_SWMATRIX, .chain = 0, .n_sel = 0, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { GM_NO_PIN, GM_NO_PIN, GM_NO_PIN, GM_NO_PIN },
        .common = GM_NO_PIN, .enable = GM_NO_PIN,
        .settle_us = 4,
        .rows = hub8_rows, .cols = hub8_cols, .n_rows = 1, .n_cols = 8,
    },
    { /* 9: encoder_rows (switch_matrix) */
        .type = GM_HUB_SWMATRIX, .chain = 0, .n_sel = 0, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { GM_NO_PIN, GM_NO_PIN, GM_NO_PIN, GM_NO_PIN },
        .common = GM_NO_PIN, .enable = GM_NO_PIN,
        .settle_us = 4,
        .rows = hub9_rows, .cols = hub9_cols, .n_rows = 2, .n_cols = 8,
    },
};

static const gm_button_t buttons[GM_N_BUTTONS] = {
    { { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 41 }, "Sustain", -1 }, /* sustain: PC9 */
    { { 7, 0, 0 }, "Up", -1 }, /* up: panel.0 */
    { { 7, 0, 1 }, "Down", -1 }, /* down: panel.1 */
    { { 7, 0, 2 }, "Shift", -1 }, /* shift: panel.2 */
    { { 7, 0, 3 }, "Channel", -1 }, /* channel: panel.3 */
    { { 7, 0, 4 }, "Edit", -1 }, /* edit: panel.4 */
    { { 7, 0, 5 }, "Zone", -1 }, /* zone: panel.5 */
    { { 7, 0, 6 }, "1", -1 }, /* num_1: panel.6 */
    { { 7, 0, 7 }, "2", -1 }, /* num_2: panel.7 */
    { { 7, 0, 8 }, "3", -1 }, /* num_3: panel.8 */
    { { 7, 0, 9 }, "4", -1 }, /* num_4: panel.9 */
    { { 7, 0, 10 }, "Rewind", -1 }, /* rewind: panel.10 */
    { { 7, 0, 11 }, "Fast forward", -1 }, /* fast_forward: panel.11 */
    { { 7, 0, 12 }, "Stop", -1 }, /* stop: panel.12 */
    { { 7, 0, 13 }, "Play", -1 }, /* play: panel.13 */
    { { 7, 0, 14 }, "Record", -1 }, /* record: panel.14 */
    { { 7, 0, 15 }, "Pad octave -", -1 }, /* pad_octave_down: panel.15 */
    { { 7, 0, 16 }, "Pad octave +", -1 }, /* pad_octave_up: panel.16 */
    { { 7, 0, 17 }, "Transpose", -1 }, /* transpose: panel.17 */
    { { 7, 0, 18 }, "Octave -", -1 }, /* octave_down: panel.18 */
    { { 7, 0, 19 }, "Octave +", -1 }, /* octave_up: panel.19 */
    { { 7, 0, 20 }, "Slot 20", -1 }, /* slot_20: panel.20 */
    { { 7, 0, 21 }, "Slot 21", -1 }, /* slot_21: panel.21 */
    { { 7, 0, 22 }, "Slot 22", -1 }, /* slot_22: panel.22 */
    { { 7, 0, 23 }, "Slot 23", -1 }, /* slot_23: panel.23 */
    { { 7, 0, 24 }, "Slot 24", -1 }, /* slot_24: panel.24 */
    { { 7, 0, 25 }, "Slot 25", -1 }, /* slot_25: panel.25 */
    { { 7, 0, 26 }, "Slot 26", -1 }, /* slot_26: panel.26 */
    { { 7, 0, 27 }, "Slot 27", -1 }, /* slot_27: panel.27 */
    { { 7, 0, 28 }, "Slot 28", -1 }, /* slot_28: panel.28 */
    { { 7, 0, 29 }, "Slot 29", -1 }, /* slot_29: panel.29 */
    { { 7, 0, 30 }, "Slot 30", -1 }, /* slot_30: panel.30 */
    { { 7, 0, 31 }, "Slot 31", -1 }, /* slot_31: panel.31 */
    { { 8, 0, 0 }, "Slot 32", -1 }, /* slot_32: panel_row20.0 */
    { { 8, 0, 1 }, "Slot 33", -1 }, /* slot_33: panel_row20.1 */
    { { 8, 0, 2 }, "Slot 34", -1 }, /* slot_34: panel_row20.2 */
    { { 8, 0, 3 }, "Slot 35", -1 }, /* slot_35: panel_row20.3 */
    { { 8, 0, 4 }, "Slot 36", -1 }, /* slot_36: panel_row20.4 */
    { { 8, 0, 5 }, "Slot 37", -1 }, /* slot_37: panel_row20.5 */
    { { 8, 0, 6 }, "Slot 38", -1 }, /* slot_38: panel_row20.6 */
    { { 8, 0, 7 }, "Slot 39", -1 }, /* slot_39: panel_row20.7 */
};

static const gm_analog_t analogs[GM_N_ANALOGS] = {
    { { 1, 0, 0 }, "Fader 1", 80, 4000 }, /* fader_1: fader_mux.0 */
    { { 1, 0, 1 }, "Fader 2", 80, 4000 }, /* fader_2: fader_mux.1 */
    { { 1, 0, 2 }, "Fader 3", 80, 4000 }, /* fader_3: fader_mux.2 */
    { { 1, 0, 3 }, "Fader 4", 80, 4000 }, /* fader_4: fader_mux.3 */
    { { 1, 0, 4 }, "Fader 5", 80, 4000 }, /* fader_5: fader_mux.4 */
    { { 1, 0, 5 }, "Fader 6", 80, 4000 }, /* fader_6: fader_mux.5 */
    { { 1, 0, 6 }, "Fader 7", 80, 4000 }, /* fader_7: fader_mux.6 */
    { { 1, 0, 7 }, "Fader 8", 80, 4000 }, /* fader_8: fader_mux.7 */
    { { 0, 0, 6 }, "Master", 80, 4000 }, /* fader_9: PA6 */
};

static const gm_encoder_t encoders[GM_N_ENCODERS] = {
    { { 9, 0, 0 }, { 9, 0, 1 }, "E1", 4 }, /* encoder_1: encoder_rows.0, encoder_rows.1 */
    { { 9, 0, 2 }, { 9, 0, 3 }, "E2", 4 }, /* encoder_2: encoder_rows.2, encoder_rows.3 */
    { { 9, 0, 4 }, { 9, 0, 5 }, "E3", 4 }, /* encoder_3: encoder_rows.4, encoder_rows.5 */
    { { 9, 0, 6 }, { 9, 0, 7 }, "E4", 4 }, /* encoder_4: encoder_rows.6, encoder_rows.7 */
    { { 9, 0, 8 }, { 9, 0, 9 }, "E5", 4 }, /* encoder_5: encoder_rows.8, encoder_rows.9 */
    { { 9, 0, 10 }, { 9, 0, 11 }, "E6", 4 }, /* encoder_6: encoder_rows.10, encoder_rows.11 */
    { { 9, 0, 12 }, { 9, 0, 13 }, "E7", 4 }, /* encoder_7: encoder_rows.12, encoder_rows.13 */
    { { 9, 0, 14 }, { 9, 0, 15 }, "E8", 4 }, /* encoder_8: encoder_rows.14, encoder_rows.15 */
};

static const gm_pin_t matrix_rows[16] = {
    { 6, 0, 0 }, /* rows.0 */
    { 6, 0, 1 }, /* rows.1 */
    { 6, 0, 2 }, /* rows.2 */
    { 6, 0, 3 }, /* rows.3 */
    { 6, 0, 4 }, /* rows.4 */
    { 6, 0, 5 }, /* rows.5 */
    { 6, 0, 6 }, /* rows.6 */
    { 6, 0, 7 }, /* rows.7 */
    { 6, 0, 8 }, /* rows.8 */
    { 6, 0, 9 }, /* rows.9 */
    { 6, 0, 10 }, /* rows.10 */
    { 6, 0, 11 }, /* rows.11 */
    { 6, 0, 12 }, /* rows.12 */
    { 6, 0, 13 }, /* rows.13 */
    { 6, 0, 14 }, /* rows.14 */
    { 6, 0, 15 }, /* rows.15 */
};
static const gm_pin_t matrix_cols[8] = {
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 24 }, /* PB8 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 25 }, /* PB9 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 26 }, /* PB10 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 27 }, /* PB11 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 28 }, /* PB12 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 29 }, /* PB13 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 30 }, /* PB14 */
    { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 31 }, /* PB15 */
};
static const uint8_t matrix_key_map[64] = {
    55, 54, 53, 52, 51, 50, 49, 48,
    47, 46, 45, 44, 43, 42, 41, 40,
    39, 38, 37, 36, 35, 34, 33, 32,
    31, 30, 29, 28, 27, 26, 25, 24,
    23, 22, 21, 20, 19, 18, 17, 16,
    15, 14, 13, 12, 11, 10, 9, 8,
    7, 6, 5, 4, 3, 2, 1, 0,
    255, 255, 255, 60, 59, 58, 57, 56,
};
static const gm_matrix_t matrix = {
    .rows = matrix_rows, .n_rows = 16,
    .cols = matrix_cols, .n_cols = 8,
    .kind = GM_MATRIX_KIND_VELOCITY, .n_keys = 61, .first_note = 36, .settle_us = 4,
    .contacts = GM_MATRIX_CONTACTS_ROWS, .key_map = matrix_key_map,
};

const gm_device_t gm_device = {
    .name = GM_DEVICE_NAME,
    .node_name = GM_NODE_NAME,
    .board_id = GM_BOARD_ID,
    .usb_vid = GM_USB_VID,
    .usb_pid = GM_USB_PID,
    .hubs = hubs, .n_hubs = GM_N_HUBS,
    .buttons = buttons, .n_buttons = GM_N_BUTTONS,
    .leds = 0, .n_leds = GM_N_LEDS,
    .analogs = analogs, .n_analogs = GM_N_ANALOGS,
    .encoders = encoders, .n_encoders = GM_N_ENCODERS,
    .matrix = &matrix,
    .boot_combo = 0,
    .scan_us = 250,
};

/* param defaults; button.action values: core/README.md "Button actions" */
const int32_t gm_button_action_defaults[GM_N_BUTTONS] = { 1, 11, 12, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; /* cc_momentary, program_next, program_prev, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, cc_momentary, octave_down, octave_up, none, none, none, none, none, none, none, none, none, none, none, none, none, none, none, none, none, none, none, none */
const int32_t gm_button_number_defaults[GM_N_BUTTONS] = { 64, 0, 0, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
const int32_t gm_analog_cc_defaults[GM_N_ANALOGS] = { 20, 21, 22, 23, 24, 25, 26, 27, 7 };
const int32_t gm_encoder_cc_defaults[GM_N_ENCODERS] = { 70, 71, 72, 73, 74, 75, 76, 77 };

const gm_ring_t gm_encoder_ring[GM_N_ENCODERS] = { { 0, 0, GM_RING_NO_CENTER }, { 0, 0, GM_RING_NO_CENTER }, { 0, 0, GM_RING_NO_CENTER }, { 0, 0, GM_RING_NO_CENTER }, { 0, 0, GM_RING_NO_CENTER }, { 0, 0, GM_RING_NO_CENTER }, { 0, 0, GM_RING_NO_CENTER }, { 0, 0, GM_RING_NO_CENTER } };

/* DAW-script component descriptions: { id, name, role } (sysex.md 4.6.1) */
const gm_comp_t gm_comp_buttons[GM_N_BUTTONS] = { { "sustain", "Sustain", "" }, { "up", "Up", "" }, { "down", "Down", "" }, { "shift", "Shift", "" }, { "channel", "Channel", "" }, { "edit", "Edit", "" }, { "zone", "Zone", "" }, { "num_1", "1", "" }, { "num_2", "2", "" }, { "num_3", "3", "" }, { "num_4", "4", "" }, { "rewind", "Rewind", "" }, { "fast_forward", "Fast forward", "" }, { "stop", "Stop", "" }, { "play", "Play", "" }, { "record", "Record", "" }, { "pad_octave_down", "Pad octave -", "" }, { "pad_octave_up", "Pad octave +", "" }, { "transpose", "Transpose", "" }, { "octave_down", "Octave -", "" }, { "octave_up", "Octave +", "" }, { "slot_20", "Slot 20", "" }, { "slot_21", "Slot 21", "" }, { "slot_22", "Slot 22", "" }, { "slot_23", "Slot 23", "" }, { "slot_24", "Slot 24", "" }, { "slot_25", "Slot 25", "" }, { "slot_26", "Slot 26", "" }, { "slot_27", "Slot 27", "" }, { "slot_28", "Slot 28", "" }, { "slot_29", "Slot 29", "" }, { "slot_30", "Slot 30", "" }, { "slot_31", "Slot 31", "" }, { "slot_32", "Slot 32", "" }, { "slot_33", "Slot 33", "" }, { "slot_34", "Slot 34", "" }, { "slot_35", "Slot 35", "" }, { "slot_36", "Slot 36", "" }, { "slot_37", "Slot 37", "" }, { "slot_38", "Slot 38", "" }, { "slot_39", "Slot 39", "" } };
const gm_comp_t gm_comp_analogs[GM_N_ANALOGS] = { { "fader_1", "Fader 1", "" }, { "fader_2", "Fader 2", "" }, { "fader_3", "Fader 3", "" }, { "fader_4", "Fader 4", "" }, { "fader_5", "Fader 5", "" }, { "fader_6", "Fader 6", "" }, { "fader_7", "Fader 7", "" }, { "fader_8", "Fader 8", "" }, { "fader_9", "Master", "" } };
const gm_comp_t gm_comp_encoders[GM_N_ENCODERS] = { { "encoder_1", "E1", "" }, { "encoder_2", "E2", "" }, { "encoder_3", "E3", "" }, { "encoder_4", "E4", "" }, { "encoder_5", "E5", "" }, { "encoder_6", "E6", "" }, { "encoder_7", "E7", "" }, { "encoder_8", "E8", "" } };
const gm_comp_t gm_comp_leds[1] = { { "", "", "" } };
const gm_comp_t gm_comp_matrix[1] = { { "keys", "Keys", "" } };

/* presets: overrides of the slot 1 values, as { param id, index, value } (ids: gm_settings.h) */
const gm_preset_def_t gm_presets[GM_N_PRESETS] = {
    { "Default", GM_PROGRAM_NONE, 0, 0 }, /* slot 1 */
};
