/* gm_device_config: do not edit; generated from devices/esp32-example.yaml
 * by tools/builder/gemifi_build.py (format: core/README.md). */

#include "gm_device.h"
#include "gm_device_config.h"

static const gm_hub_t hubs[GM_N_HUBS] = {
    { /* 1: faders (74hc4051) */
        .type = GM_HUB_MUX, .chain = 0, .n_sel = 3, .i2c_addr = 0x00,
        .data = GM_NO_PIN, .clock = GM_NO_PIN, .latch = GM_NO_PIN,
        .sel = { { 0, 0, 4 }, { 0, 0, 5 }, { 0, 0, 6 }, GM_NO_PIN },
        .common = { 0, 0, 1 }, .enable = GM_NO_PIN,
        .settle_us = 5,
    },
};

static const gm_button_t buttons[GM_N_BUTTONS] = {
    { { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 9 }, "Button 1", 0 }, /* GPIO9 */
    { { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 10 }, "Button 2", -1 }, /* GPIO10 */
    { { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 11 }, "Button 3", -1 }, /* GPIO11 */
    { { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 12 }, "Button 4", -1 }, /* GPIO12 */
};

static const gm_led_t leds[GM_N_LEDS] = {
    { { 0, 0, 21 }, GM_NO_PIN, "LED" }, /* GPIO21 */
};

static const gm_analog_t analogs[GM_N_ANALOGS] = {
    { { 0, 0, 7 }, "Pot 1", 0, 4095 }, /* GPIO7 */
    { { 0, 0, 8 }, "Pot 2", 0, 4095 }, /* GPIO8 */
    { { 1, 0, 0 }, "Fader 1", 0, 4095 }, /* faders.0 */
    { { 1, 0, 1 }, "Fader 2", 0, 4095 }, /* faders.1 */
    { { 1, 0, 2 }, "Fader 3", 0, 4095 }, /* faders.2 */
    { { 1, 0, 3 }, "Fader 4", 0, 4095 }, /* faders.3 */
    { { 1, 0, 4 }, "Fader 5", 0, 4095 }, /* faders.4 */
    { { 1, 0, 5 }, "Fader 6", 0, 4095 }, /* faders.5 */
    { { 1, 0, 6 }, "Fader 7", 0, 4095 }, /* faders.6 */
    { { 1, 0, 7 }, "Fader 8", 0, 4095 }, /* faders.7 */
};

static const gm_encoder_t encoders[GM_N_ENCODERS] = {
    { { 0, GM_PIN_PULLUP, 15 }, { 0, GM_PIN_PULLUP, 16 }, "Encoder", 4 }, /* GPIO15, GPIO16 */
};

const gm_device_t gm_device = {
    .name = GM_DEVICE_NAME,
    .board_id = GM_BOARD_ID,
    .usb_vid = GM_USB_VID,
    .usb_pid = GM_USB_PID,
    .hubs = hubs, .n_hubs = GM_N_HUBS,
    .buttons = buttons, .n_buttons = GM_N_BUTTONS,
    .leds = leds, .n_leds = GM_N_LEDS,
    .analogs = analogs, .n_analogs = GM_N_ANALOGS,
    .encoders = encoders, .n_encoders = GM_N_ENCODERS,
    .matrix = 0,
    .boot_combo = (1u << 0) | (1u << 3),
    .scan_us = 500,
};

/* param defaults; button.action values: core/README.md "Button actions" */
const int32_t gm_button_action_defaults[GM_N_BUTTONS] = { 2, 1, 5, 6 }; /* cc_toggle, cc_momentary, octave_down, octave_up */
const int32_t gm_button_number_defaults[GM_N_BUTTONS] = { 80, 64, 0, 0 };
const int32_t gm_analog_cc_defaults[GM_N_ANALOGS] = { 7, 10, 20, 21, 22, 23, 24, 25, 26, 27 };
const int32_t gm_encoder_cc_defaults[GM_N_ENCODERS] = { 30 };
