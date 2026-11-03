/*
 * gm_device.h: the hardware description a gemifi firmware is built from.
 *
 * A device is a list of components (buttons, LEDs, analog inputs, encoders,
 * a key matrix) whose pins are *virtual pins*: either a pin of the MCU
 * itself (hub 0) or a pin of an expander hub (74HC595 outputs, 74HC165
 * inputs, 74HC4051/4067 multiplexers, MCP23017 / PCF8574 over I2C, TM1628
 * LED drivers). The core
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
 * TM1628 hubs: num = display RAM bit, byte * 8 + bit (37 = byte 4, bit 5).
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
#define GM_PIN_OPEN_DRAIN (1u << 3)  /* MCU outputs: open-drain (TM1628 DIO)     */
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
    GM_HUB_DECODER = 6,   /* 74HC138-style one-of-N select: outputs only, and
                             only as matrix rows. Output n "on" = address
                             lines = n % 2^n_sel, enable lines = group
                             n / 2^n_sel's levels. "Off" leaves it as is: the
                             next row's select replaces it                 */
    GM_HUB_TM1628 = 7,    /* TM1628-class LED driver: display RAM bits,
                             outputs; written from the main loop          */
    GM_HUB_SWMATRIX = 8,  /* switch matrix scanned from the scan interrupt:
                             inputs only. Pin n = row n / n_cols, column
                             n % n_cols; reads 1 = closed                   */
    GM_HUB_RGB_SERIAL = 9, /* RGB LEDs behind a serial link (the M-Audio Code
                             pad LED board, 115200 baud on `data`): outputs
                             only, pin n = LED n / 3, colour n % 3 (0 red,
                             1 green, 2 blue); sent from the main loop     */
};

typedef struct {
    uint8_t type;
    uint8_t chain;        /* HC595/HC165: chips in the chain (8 pins each);
                             RGB_SERIAL: LEDs                              */
    uint8_t n_sel;        /* MUX: select lines (3 = 4051, 4 = 4067)        */
    uint8_t i2c_addr;     /* MCP23017 / PCF8574                            */
    gm_pin_t data;        /* HC595 SER, HC165 QH, TM1628 DIO, RGB_SERIAL TX */
    gm_pin_t clock;       /* HC595 SRCLK, HC165 CLK, TM1628 CLK            */
    gm_pin_t latch;       /* HC595 RCLK, HC165 SH/LD (load when low),
                             TM1628 STB                                    */
    gm_pin_t sel[4];      /* MUX S0..S3                                    */
    gm_pin_t common;      /* MUX common pin (an MCU pin; ADC for analog)   */
    gm_pin_t enable;      /* MUX /E, HC595 /OE (optional)                  */
    uint16_t settle_us;   /* MUX: wait after switching channel; SWMATRIX:
                             after driving a row                            */
    /* DECODER: sel[0..n_sel-1] are the address lines (A0 first). */
    gm_pin_t en[3];       /* DECODER: group enable lines                   */
    uint8_t n_en;
    uint8_t n_groups;     /* DECODER: groups of 2^n_sel outputs            */
    uint8_t group_lv[4];  /* DECODER: bit e = level of en[e] for group g    */
    /* SWMATRIX: rows (any output pin, usually a DECODER's) and columns
     * (MCU inputs; "closed" level = not INVERT). n_rows * n_cols <= 32. */
    const gm_pin_t *rows;
    const gm_pin_t *cols;
    uint8_t n_rows;
    uint8_t n_cols;
    uint8_t ram_bytes;    /* TM1628: display RAM bytes written (1..16);
                             RGB_SERIAL: bytes of LED bits (3 per LED)     */
    uint8_t brightness;   /* TM1628: 0..7 (display control 0x88 | it)      */
    uint8_t grids;        /* TM1628: display mode 4..7 grids, 0 = not sent */
    uint8_t rows_per_scan; /* SWMATRIX: rows read per scan, in turn; 0 = all */
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

/* An LED with one, two or three pins. Two pins = a bicolour LED shown with
 * levels: 1 = pin, 2 = both, 3 = pin2, 4 = pin2 blinking. Three pins = an
 * RGB LED (pin red, pin2 green, pin3 blue): any level is white, except the
 * colours FB_LED_RGB sets (controller.c LV_RGB).
 * looks (YAML idle: / active:): which pins are lit when the LED is off
 * (idle) and for a plain "on" (active), each a mask (bit 0 pin, 1 pin2,
 * 2 pin3); 0 = the defaults: idle dark, active pin (white on RGB). */
#define GM_LED_LOOKS(idle, active) (0x80u | (idle) | (active) << 3)
typedef struct {
    gm_pin_t pin;
    gm_pin_t pin2;        /* LEDs have no name: nothing shows it */
    gm_pin_t pin3;        /* RGB: blue; GM_NO_PIN otherwise */
    uint8_t looks;        /* GM_LED_LOOKS(idle, active), 0 = defaults */
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
    uint8_t contacts;      /* VELOCITY: GM_MATRIX_CONTACTS_*                */
    /* Optional: key number of each matrix position, 0xFF = none. Position =
     * row * n_cols + col for SWITCH; pair p * n_cols + col for VELOCITY with
     * contacts on rows (pair p = rows 2p, 2p+1); row * (n_cols / 2) + p for
     * contacts on columns. NULL: the default numbering above. */
    const uint8_t *key_map;
} gm_matrix_t;

enum {
    GM_MATRIX_CONTACTS_COLS = 0,  /* key k on row k % rows, cols 2p, 2p+1      */
    GM_MATRIX_CONTACTS_ROWS = 1,  /* key k = p + (rows / 2) * c: rows 2p, 2p+1,
                                     column c (the Code 61's keybed)          */
};
#define GM_KEY_NONE 0xFFu

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
extern const int32_t gm_analog_mode_defaults[];   /* AN_MODE_* (gm_settings.h) */
extern const int32_t gm_encoder_cc_defaults[];

/* Feedback / MCU tables (see above), each sized by its component count. */
extern const gm_led_fb_t gm_led_fb[];        /* GM_LED_FB: per LED            */
extern const uint8_t gm_analog_pickup[];     /* GM_ANALOG_PICKUP: 1 = pick-up */
extern const uint8_t gm_analog_mcu[];        /* GM_MCU: fader 0..7, master 8, or GM_MCU_NONE */
extern const uint8_t gm_encoder_mcu[];       /* GM_MCU: V-Pot 0..7, or GM_MCU_NONE */
extern const gm_ring_t gm_encoder_ring[];    /* GM_RINGS: ring LEDs per encoder */

/* GET_COMPONENT descriptions (GM_HOST_FB, sysex.md 4.6.1): the YAML id, the
 * display name and the DAW role (daw:, else the mcu: name, else ""). */
typedef struct {
    const char *id;
    const char *name;
    const char *role;
} gm_comp_t;
extern const gm_comp_t gm_comp_buttons[];
extern const gm_comp_t gm_comp_analogs[];
extern const gm_comp_t gm_comp_encoders[];
extern const gm_comp_t gm_comp_leds[];
extern const gm_comp_t gm_comp_matrix[];     /* one entry: the key matrix */

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

/* Modes (research/core/presets-design.md §6, core/README.md "Modes"):
 * layers that turn the keys into function keys. Generated only when the
 * YAML has modes: (GM_N_MODES > 0 in gm_device_config.h). A root mode is
 * entered and left with its toggle button; a sub-mode (parent) is entered
 * from a key and goes back to its parent after one action. */
enum {                           /* gm_mode_key_t.op                      */
    GM_MK_PRESET = 1,            /* arg = slot 1..GM_N_PRESETS            */
    GM_MK_PRESET_NEXT,
    GM_MK_PRESET_PREV,
    GM_MK_OCTAVE_DOWN,
    GM_MK_OCTAVE_UP,
    GM_MK_OCTAVE_RESET,
    GM_MK_TRANSPOSE_DOWN,
    GM_MK_TRANSPOSE_UP,
    GM_MK_TRANSPOSE_RESET,
    GM_MK_CHANNEL,               /* arg = 1..16                           */
    GM_MK_CURVE,                 /* arg = velocity.curve value            */
    GM_MK_MODE,                  /* arg = mode index: go to that sub-mode */
};
#define GM_MODE_NONE 0xFFu

typedef struct {
    uint8_t key;                 /* key matrix index (the physical key)   */
    uint8_t op;                  /* GM_MK_*                               */
    uint8_t arg;
} gm_mode_key_t;

typedef struct {
    uint8_t toggle;              /* button index; GM_MODE_NONE: sub-mode  */
    uint8_t parent;              /* mode index, or GM_MODE_NONE           */
    uint8_t root;                /* the root mode it belongs to (itself)  */
    int8_t led;                  /* lit while active, or -1               */
    uint8_t led_blink;           /* 1: led blinks instead                 */
    int8_t blink_count;          /* preset switch: this LED blinks the slot number, or -1 */
    int8_t blink_done;           /* ... then it and this one flash once, or -1 */
    uint8_t n_keys;
    const gm_mode_key_t *keys;   /* sorted by key                         */
} gm_mode_t;

extern const gm_mode_t gm_modes[];           /* GM_N_MODES entries */

/* Pads (core/README.md "Pads"): pressure pads (FSR style) that play notes
 * with a velocity from how hard they are hit. Generated only when the YAML
 * has pads: (GM_N_PADS > 0). The note and the sensing (thresholds, peak
 * window, retrigger guard, curve) are params: gm_settings.h P_PAD_*. */
typedef struct {
    gm_pin_t pin;                /* MCU ADC pin, or a MUX hub channel; INVERT = 4095 - reading */
    const char *name;
    int8_t led;                  /* index into leds, lit while the pad is held, or -1 */
} gm_pad_t;

extern const gm_pad_t gm_pads[];             /* GM_N_PADS entries */
extern const int32_t gm_pad_note_defaults[]; /* GM_N_PADS entries */

#ifdef __cplusplus
}
#endif

#endif
