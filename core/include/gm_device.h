/*
 * gm_device.h: the hardware description a gemifi firmware is built from.
 *
 * A device is a list of components (buttons, LEDs, analog inputs, encoders,
 * a key matrix) whose pins are *virtual pins*: either a pin of the MCU
 * itself (hub 0) or a pin of an expander hub (74HC595 outputs, 74HC165
 * inputs, 74HC4051/4067 multiplexers, MCP23017 / PCF8574 over I2C). The core
 * drives the hubs; the platform HAL only knows its own pins.
 *
 * The tables are generated from a device YAML by tools/builder (see
 * core/README.md), into gm_device_config.h (counts, as #defines, so the
 * param table can be sized at compile time) and gm_device_config.c (the
 * const gm_device). Plain C, no platform code.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_DEVICE_H
#define GEMIFI_GM_DEVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- virtual pins ----------------------------------------------------- */
/* hub 0 = MCU pin (num = platform pin number: NUC1xx port * 16 + bit, e.g.
 * PB12 = 0x1C; ESP32/ESP8266 GPIO number). hub n >= 1 = gm_device.hubs[n-1],
 * num = that hub's pin / channel (chained chips continue: 8..15 = 2nd chip).
 * GM_PIN_INVERT: the signal is active-low (pressed / lit = electrical 0). */
typedef struct {
    uint8_t hub;
    uint8_t flags;
    uint16_t num;
} gm_pin_t;

#define GM_HUB_NONE     0xFFu        /* "no pin" */
#define GM_PIN_INVERT   (1u << 0)
#define GM_PIN_PULLUP   (1u << 1)    /* MCU pins: enable the internal pull-up   */
#define GM_PIN_PULLDOWN (1u << 2)    /* MCU pins: enable the internal pull-down */
#define GM_NO_PIN       { GM_HUB_NONE, 0, 0 }

/* ---- hubs (pin expanders) --------------------------------------------- */
enum {
    GM_HUB_HC595 = 1,     /* serial-in shift register: outputs           */
    GM_HUB_HC165 = 2,     /* parallel-in shift register: inputs          */
    GM_HUB_MUX = 3,       /* 74HC4051 (3 select) / 4067 (4 select): one
                             common pin routed to channel 0..7 / 0..15;
                             analog or digital, input only                 */
    GM_HUB_MCP23017 = 4,  /* I2C, 16 GPIO                                */
    GM_HUB_PCF8574 = 5,   /* I2C, 8 quasi-bidirectional GPIO             */
};

typedef struct {
    uint8_t type;
    uint8_t chain;        /* HC595/HC165: chips in the chain (8 pins each) */
    uint8_t n_sel;        /* MUX: select lines (3 = 4051, 4 = 4067)        */
    uint8_t i2c_addr;     /* MCP23017 / PCF8574                            */
    gm_pin_t data;        /* HC595 SER, HC165 QH                           */
    gm_pin_t clock;       /* HC595 SRCLK, HC165 CLK                        */
    gm_pin_t latch;       /* HC595 RCLK, HC165 SH/LD (load when low)       */
    gm_pin_t sel[4];      /* MUX S0..S3                                    */
    gm_pin_t common;      /* MUX common pin (an MCU pin; ADC for analog)   */
    gm_pin_t enable;      /* MUX /E, HC595 /OE (optional)                  */
    uint16_t settle_us;   /* MUX: wait after switching channel             */
} gm_hub_t;

/* ---- components ------------------------------------------------------- */
typedef struct {
    gm_pin_t pin;
    const char *label;
    int8_t led;           /* index into leds, or -1                        */
} gm_button_t;

/* An LED with one or two pins. Two pins = a bicolour LED shown with
 * levels: 1 = pin, 2 = both, 3 = pin2, 4 = pin2 blinking. */
typedef struct {
    gm_pin_t pin;
    gm_pin_t pin2;
    const char *label;
} gm_led_t;

typedef struct {
    gm_pin_t pin;         /* MCU ADC pin, or a MUX hub channel             */
    const char *label;
    uint16_t raw_min;     /* raw reading (0..4095) mapped to 0             */
    uint16_t raw_max;     /* raw reading mapped to the top; < raw_min = reversed */
} gm_analog_t;

typedef struct {
    gm_pin_t a, b;
    const char *label;
    uint8_t steps_per_detent;   /* quadrature steps per click (usually 4) */
} gm_encoder_t;

enum {
    GM_MATRIX_KIND_SWITCH = 0,  /* one contact per key: key = row + rows * col  */
    GM_MATRIX_KIND_VELOCITY = 1 /* two contacts per key (velocity): key k on row
                              k % rows, cols 2p and 2p+1 with p = k / rows */
};

typedef struct {
    const gm_pin_t *rows;  /* driven one at a time ("active" level = not INVERT) */
    uint8_t n_rows;
    const gm_pin_t *cols;  /* read; "closed" level = not INVERT            */
    uint8_t n_cols;
    uint8_t kind;
    uint8_t n_keys;
    uint8_t first_note;    /* note of key 0 at octave 0, transpose 0       */
    uint8_t settle_us;     /* wait after driving a row                      */
} gm_matrix_t;

typedef struct {
    const char *name;            /* "Keystation Mini 32"                */
    uint16_t board_id;           /* sysex.md 7                          */
    uint16_t usb_vid, usb_pid;   /* platforms with native USB           */
    const gm_hub_t *hubs;
    uint8_t n_hubs;
    const gm_button_t *buttons;
    uint8_t n_buttons;
    const gm_led_t *leds;
    uint8_t n_leds;
    const gm_analog_t *analogs;
    uint8_t n_analogs;
    const gm_encoder_t *encoders;
    uint8_t n_encoders;
    const gm_matrix_t *matrix;   /* or NULL                              */
    uint32_t boot_combo;         /* buttons (bit = index) held at reset:
                                    stay in the bootloader (NUC) / safe mode */
    uint16_t scan_us;            /* matrix + encoder scan period         */
} gm_device_t;

extern const gm_device_t gm_device;

/* Per-component defaults for the params (generated alongside gm_device;
 * arrays sized by the GM_N_* counts in gm_device_config.h). */
extern const int32_t gm_button_action_defaults[];
extern const int32_t gm_button_number_defaults[];
extern const int32_t gm_analog_cc_defaults[];
extern const int32_t gm_encoder_cc_defaults[];

#ifdef __cplusplus
}
#endif

#endif
