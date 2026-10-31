/* gm_device_config: do not edit; generated from devices/esp8266-example.yaml
 * by tools/builder/gemifi_build.py (format: core/README.md). */

#include "gm_device.h"
#include "gm_device_config.h"

static const gm_button_t buttons[GM_N_BUTTONS] = {
    { { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 14 }, "Button 1", 0 }, /* GPIO14 */
    { { 0, GM_PIN_INVERT | GM_PIN_PULLUP, 12 }, "Button 2", -1 }, /* GPIO12 */
};

static const gm_led_t leds[GM_N_LEDS] = {
    { { 0, GM_PIN_INVERT, 2 }, GM_NO_PIN, "LED" }, /* GPIO2 */
};

static const gm_analog_t analogs[GM_N_ANALOGS] = {
    { { 0, 0, 17 }, "Pot", 0, 4095 }, /* A0 */
};

const gm_device_t gm_device = {
    .name = GM_DEVICE_NAME,
    .board_id = GM_BOARD_ID,
    .usb_vid = GM_USB_VID,
    .usb_pid = GM_USB_PID,
    .hubs = 0, .n_hubs = GM_N_HUBS,
    .buttons = buttons, .n_buttons = GM_N_BUTTONS,
    .leds = leds, .n_leds = GM_N_LEDS,
    .analogs = analogs, .n_analogs = GM_N_ANALOGS,
    .encoders = 0, .n_encoders = GM_N_ENCODERS,
    .matrix = 0,
    .boot_combo = (1u << 0) | (1u << 1),
    .scan_us = 1000,
};

/* param defaults; button.action values: core/README.md "Button actions" */
const int32_t gm_button_action_defaults[GM_N_BUTTONS] = { 1, 2 }; /* cc_momentary, cc_toggle */
const int32_t gm_button_number_defaults[GM_N_BUTTONS] = { 64, 80 };
const int32_t gm_analog_cc_defaults[GM_N_ANALOGS] = { 7 };
const int32_t gm_encoder_cc_defaults[1] = { 0 };
