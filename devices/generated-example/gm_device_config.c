/* Generated from devices/keystation-mini32-mk3.yaml (hand-written reference
 * for tools/builder; the builder's output must match it). */
#include "gm_device.h"
#include "gm_device_config.h"

#define P(port, bit) ((port) * 16 + (bit))
#define PA 0
#define PB 1
#define PC 2
#define MCU(n, f) { 0, (f), (n) }
#define INV GM_PIN_INVERT

static const gm_button_t buttons[GM_N_BUTTONS] = {
    { MCU(P(PA, 1), INV), "Sustain", 0 },
    { MCU(P(PA, 2), INV), "Edit", 1 },
    { MCU(P(PA, 3), INV), "Oct -", 2 },
    { MCU(P(PA, 4), INV), "Oct +", 3 },
    { MCU(P(PA, 5), INV), "Pitch down", -1 },
    { MCU(P(PA, 6), INV), "Pitch up", -1 },
    { MCU(P(PA, 7), INV), "Mod", -1 },
};

static const gm_led_t leds[GM_N_LEDS] = {
    { MCU(P(PA, 12), INV), GM_NO_PIN, "Sustain LED" },
    { MCU(P(PA, 13), INV), GM_NO_PIN, "Edit LED" },
    { MCU(P(PA, 10), INV), MCU(P(PA, 14), INV), "Oct - LED" },
    { MCU(P(PA, 11), INV), MCU(P(PA, 15), INV), "Oct + LED" },
};

static const gm_analog_t analogs[GM_N_ANALOGS] = {
    { MCU(P(PA, 0), 0), "Volume", 32, 4064 },
};

static const gm_pin_t rows[8] = {
    MCU(P(PA, 8), 0), MCU(P(PA, 9), 0), MCU(P(PC, 0), 0), MCU(P(PC, 1), 0),
    MCU(P(PC, 2), 0), MCU(P(PC, 3), 0), MCU(P(PC, 6), 0), MCU(P(PC, 7), 0),
};
static const gm_pin_t cols[8] = {
    MCU(P(PB, 0), 0), MCU(P(PB, 1), 0), MCU(P(PB, 2), 0), MCU(P(PB, 3), 0),
    MCU(P(PB, 4), 0), MCU(P(PB, 5), 0), MCU(P(PB, 8), 0), MCU(P(PB, 12), 0),
};
static const gm_matrix_t matrix = {
    rows, 8, cols, 8, GM_MATRIX_KIND_VELOCITY, 32, 48, 1,
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
    .matrix = &matrix,
    .boot_combo = (1u << 4) | (1u << 5),
    .scan_us = 125,
};

/* button.action values: core/README.md "Button actions" */
const int32_t gm_button_action_defaults[GM_N_BUTTONS] = { 1, 2, 5, 6, 9, 10, 3 };
const int32_t gm_button_number_defaults[GM_N_BUTTONS] = { 64, 80, 0, 0, 0, 0, 1 };
const int32_t gm_analog_cc_defaults[GM_N_ANALOGS] = { 7 };
const int32_t gm_encoder_cc_defaults[1] = { 0 };
