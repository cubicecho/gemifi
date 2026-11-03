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
/* name: the display name (YAML name:, or derived from the id): the index
 * label of the button's params in the schema, and the debug log. Ids are a
 * build-time thing and don't reach the firmware. */
typedef struct {
    gm_pin_t pin;
    const char *name;
    int8_t led;           /* index into leds, or -1                        */
} gm_button_t;

/* An LED with one or two pins. Two pins = a bicolour LED shown with
 * levels: 1 = pin, 2 = both, 3 = pin2, 4 = pin2 blinking. */
typedef struct {
    gm_pin_t pin;
    gm_pin_t pin2;        /* LEDs have no name: nothing shows it */
} gm_led_t;

typedef struct {
    gm_pin_t pin;         /* MCU ADC pin, or a MUX hub channel             */
    const char *name;
    uint16_t raw_min;     /* raw reading (0..4095) mapped to 0             */
    uint16_t raw_max;     /* raw reading mapped to the top; < raw_min = reversed */
} gm_analog_t;

typedef struct {
    gm_pin_t a, b;
    const char *name;
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

/* ---- host feedback and Mackie Control --------------------------------
 * core/README.md "DAW feedback". These tables are generated only when the
 * YAML uses them (GM_LED_FB, GM_ANALOG_PICKUP, GM_MCU in gm_device_config.h),
 * so a device without them pays nothing. */
enum {                      /* gm_led_fb_t.source: what the host drives it with */
    GM_FB_NONE = 0,         /* only its button's echo (if a button has it)   */
    GM_FB_NOTE = 1,         /* Note On/Off num on channel; velocity = level  */
    GM_FB_CC = 2,           /* CC num on channel; value = level              */
    GM_FB_CLOCK = 3,        /* MIDI clock: lit for the first 16th of a beat  */
    GM_FB_PLAYING = 4,      /* Start / Continue = on, Stop = off             */
    GM_FB_MCU = 5,          /* Mackie Control LED: Note On num, MCU port     */
};
enum {                      /* who drives an LED that both sides want         */
    GM_OWNER_HOST_THEN_LOCAL = 0,   /* host, until timeout after its last word */
    GM_OWNER_HOST = 1,              /* only the host                           */
    GM_OWNER_LOCAL = 2,             /* only the device (host ignored)          */
};
#define GM_TIMEOUT_NEVER 0xFFFFu

typedef struct {
    uint8_t source;
    uint8_t num;            /* note / CC / MCU note number                   */
    uint8_t channel;        /* 1..16; 0 = the midi.channel param             */
    uint8_t owner;
    uint16_t timeout;       /* host_then_local: 10 ms units, or GM_TIMEOUT_NEVER */
} gm_led_fb_t;

/* LEDs around an MCU V-Pot (encoder), leftmost first, and the LED under
 * it (the ring value's bit 6), or GM_RING_NO_CENTER. */
#define GM_RING_NO_CENTER 0xFFu
typedef struct {
    const uint8_t *leds;
    uint8_t n;
    uint8_t center;
} gm_ring_t;

#define GM_MCU_NONE 0xFFu

typedef struct {
    const char *name;            /* "Keystation Mini 32"                */
    const char *node_name;       /* "keystation-mini32" (HELLO node, sysex.md 4.1.1); NULL = none */
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

/* Feedback / MCU tables (see above), each sized by its component count. */
extern const gm_led_fb_t gm_led_fb[];        /* GM_LED_FB: per LED            */
extern const uint8_t gm_analog_pickup[];     /* GM_ANALOG_PICKUP: 1 = pick-up */
extern const uint8_t gm_analog_mcu[];        /* GM_MCU: fader 0..7, master 8, or GM_MCU_NONE */
extern const uint8_t gm_encoder_mcu[];       /* GM_MCU: V-Pot 0..7, or GM_MCU_NONE */
extern const gm_ring_t gm_encoder_ring[];    /* GM_MCU: ring LEDs per encoder */

/* Presets (research/core/presets-design.md §2, §10.1). Always generated:
 * GM_N_PRESETS (>= 1) slots and GM_PRESETS_REVISION in gm_device_config.h.
 * Slot n is gm_presets[n - 1]. Slot 1 is the component defaults (no
 * overrides); each other YAML preset lists only the values that differ from
 * slot 1; empty slots are named "Preset N" with no overrides. */
typedef struct {
    uint16_t id;                 /* param id (gm_settings.h P_*)         */
    uint16_t index;              /* component index (0 for plain params) */
    int32_t value;
} gm_preset_val_t;

#define GM_PROGRAM_NONE (-1)

typedef struct {
    const char *name;            /* <= 16 ASCII characters               */
    int16_t program;             /* 0..127, or GM_PROGRAM_NONE           */
    uint16_t n_vals;
    const gm_preset_val_t *vals; /* NULL when n_vals is 0                */
} gm_preset_def_t;

extern const gm_preset_def_t gm_presets[];   /* GM_N_PRESETS entries */

#ifdef __cplusplus
}
#endif

#endif
