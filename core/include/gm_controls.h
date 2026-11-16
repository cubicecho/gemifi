/*
 * gm_controls.h: what the controller (core/src/controller.c) and the control
 * components (components/buttons, leds, pads ...) share: the helpers every
 * control sends with, the state one control keeps and another reads, and
 * what the main loop calls. Each shared variable's comment names the
 * components that write it. Not for the platforms (gm_controller.h is
 * theirs).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GM_CONTROLS_H
#define GM_CONTROLS_H

#include <stdint.h>
#include "gm_device_config.h"
#include "app_proto.h"                  /* GM_MONITOR, EV_* */
#include "gm_device.h"
#include "gm_settings.h"                /* GM_MSG_WIDE, GM_N_SYSEX, K_* */
#include "gm_controller.h"              /* gm_scan_plan_t */
#include "gm_feedback.h"                /* GM_FEEDBACK, GM_LED_FB, GM_HOST_LEDS ... */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GM_N_MODES
#define GM_N_MODES 0                    /* a config generated before modes */
#endif
/* Modes need a button to enter them; only a mode's keys: (function keys)
 * need the key matrix, and the builder checks that (components/modes/README.md "Modes"). */
#if GM_N_MODES > 0 && GM_N_BUTTONS == 0
#error "modes need a toggle, hold or cycle button (the builder checks this)"
#endif
#ifndef GM_LAYERS
#define GM_LAYERS 0                     /* layer twins (modes: layer:), components/modes/README.md "Button layers" */
#endif
#if GM_LAYERS && GM_N_MODES == 0
#error "button layers belong to modes (the builder checks this)"
#endif
#ifndef GM_MODE_ONE_SHOT
#define GM_MODE_ONE_SHOT 0              /* a one_shot: mode, components/modes/README.md "One-shot layers" */
#endif
#ifndef GM_PAD_TWINS
#define GM_PAD_TWINS 0                  /* a config generated before pad twins */
#endif
#ifndef GM_HW_SCAN
#define GM_HW_SCAN 0                    /* a config generated before hardware_scan: */
#endif
#ifndef GM_TM_FADE
#define GM_TM_FADE 0                    /* no tm1628 hub with fade: (gm_device_config.h) */
#endif
#ifndef GM_STATUS_LED
#define GM_STATUS_LED (-1)              /* status_led: (gm_safety.h) */
#endif
#ifndef GM_MODE_RGB
#define GM_MODE_RGB 0                   /* a cycle step with a full colour: gm_mode_rgb[] */
#endif
#ifndef GM_ENC_ACCEL
#define GM_ENC_ACCEL 0                  /* a config generated before acceleration: */
#endif
#ifndef GM_BTN_CTL
#define GM_BTN_CTL 0                    /* gm_button_ctl[]: channel:, min:, max: */
#endif
#ifndef GM_ANALOG_CTL
#define GM_ANALOG_CTL 0                 /* gm_analog_ctl[] */
#endif
#ifndef GM_ENC_CTL
#define GM_ENC_CTL 0                    /* gm_encoder_ctl[] */
#endif
#ifndef GM_BTN_BANK
#define GM_BTN_BANK 0                   /* gm_button_bank[] */
#endif
#define GM_CTL (GM_BTN_CTL || GM_ANALOG_CTL || GM_ENC_CTL)
/* Analog inputs and encoders that can send more than a 7-bit CC. */
#define GM_MSG_KINDS (GM_MSG_WIDE || GM_N_SYSEX)

/* Array sizes per control (at least 1, GM_ARR). */
#define NB GM_ARR(GM_N_BUTTONS)
#define NA GM_ARR(GM_N_ANALOGS)
#define NE GM_ARR(GM_N_ENCODERS)
#define NK GM_ARR(GM_MATRIX_KEYS)
/* Encoders with pins of their own; their layer twins (GM_ENC_TWINS) come
 * after them and aren't scanned. */
#define NE_HW (GM_N_ENCODERS - GM_ENC_TWINS)
#define NOTE_NONE 0xFFu                 /* a key or button with no note sounding */

/* ---- MIDI out (controller.c) ------------------------------------------- */

uint8_t midi_chan(void);                /* the device's MIDI channel, 0-15 (midi.channel - 1) */
void midi3(uint8_t status, uint32_t a, uint32_t b);
void send_event(uint8_t type, uint32_t index, uint32_t value);   /* an EVENT, with debug.events on */
#if GM_N_BUTTONS > 0 || GM_N_ANALOGS > 0
void midi2(uint8_t status, uint32_t a);
#endif

/* A CC on midi.channel (a control without channel:). */
static inline void cc(uint32_t num, uint32_t value)
{
    midi3(0xB0 | midi_chan(), num, value);
}

/* ---- MIDI messages (controller.c, core/README.md "MIDI messages") ------ */

#if GM_CTL
uint8_t ctl_chan(const gm_ctl_t *t);    /* a control's channel: its channel:, else midi.channel */
int32_t ctl_hi(const gm_ctl_t *t, int32_t top);
uint32_t ctl_out(const gm_ctl_t *t, int32_t v, int32_t top);   /* v of 0..top as the control's min..max */
#endif
#if GM_MSG_KINDS
int kind_wide(uint32_t kind, uint32_t num);     /* a kind (K_*) whose values are 14-bit */
/* Value v (7- or 14-bit, as kind wants) as kind on channel ch: num is the
 * CC, the (N)RPN number or the SysEx index. */
void send_value(uint32_t kind, uint8_t ch, uint32_t num, uint32_t v);
#endif
#if GM_N_SYSEX || GM_MSG_MMC
void sysex_out(const uint8_t *m, uint32_t n);
#endif
#if GM_N_SYSEX
void sysex_send(uint32_t k, uint32_t v, uint8_t ch);   /* SysEx string k with value v and channel ch filled in */
#endif

/* ---- buttons (components/buttons) -------------------------------------- */

#if GM_N_BUTTONS > 0
#define BTN_SAMPLE_MS 1u                /* gm_poll samples the buttons every BTN_SAMPLE_MS */
/* A press counts at the first sample that read it closed throughout (every
 * scan since the sample before: 1 ms), so a quick tap is never lost; a
 * release counts once no scan has read it closed for BTN_STABLE samples in a
 * row (16 ms), which rides out contact bounce and a light press losing
 * contact for a moment. */
#define BTN_STABLE 16u
/* A reading of every button, one bit each, in the narrowest type that holds
 * GM_N_BUTTONS (64-bit shifts are library calls on a Cortex-M0). */
#if GM_N_BUTTONS <= 8
typedef uint8_t btn_bits_t;
#elif GM_N_BUTTONS <= 16
typedef uint16_t btn_bits_t;
#elif GM_N_BUTTONS <= 32
typedef uint32_t btn_bits_t;
#else
typedef uint64_t btn_bits_t;
#endif
/* A button's channel (channel:). */
#if GM_BTN_CTL
#define BTN_CH(i) ctl_chan(&gm_button_ctl[i])
#else
#define BTN_CH(i) midi_chan()
#endif
extern uint64_t btn_stable;             /* buttons: pressed, debounced (bit = button, at most 64) */
extern uint64_t btn_toggled;            /* buttons, and host feedback: a cc_toggle button's state */
/* Held, and the press went to this button: a layer twin and its base read
 * the same pin, but only the live one takes the press (and its release). */
extern uint64_t btn_held;               /* buttons */
#if GM_LAYERS
extern uint64_t btn_live;               /* modes (layers_update): the buttons that take a press */
#endif
extern btn_bits_t btn_hist[BTN_STABLE - 1u];    /* buttons: the samples before the latest; the raw readings */
extern int16_t ramp_q8[NB];             /* buttons: CC ramp position, 0..127 << 8; the LEDs show it */
/* *v + step, within -lim..lim (0 with reset), and its name and value on the
 * display: the octave, transpose and pad octave buttons and keys. */
void step_and_show(int32_t *v, int32_t step, int32_t lim, uint16_t id, int reset);
void buttons_init(void);
void buttons_sample(void);              /* every BTN_SAMPLE_MS: presses and releases */
void ramps_ms(uint32_t now);            /* every ms: the CC ramps, the pitch bend glide */
void buttons_off(void);                 /* a preset switch: let go of every note and CC */
#endif

/* ---- modes (components/modes, components/modes/README.md "Modes") ------------------ */

#if GM_N_MODES > 0
#if GM_HAS_MATRIX
uint32_t keys_mode(void);               /* the mode the keys go to; GM_MODE_NONE = the keys play */
void mode_key(uint32_t mi, uint32_t k); /* key k pressed in mode mi: its binding */
#endif
/* What LED li shows instead of level: a mode's LED, a preset number's blink. */
uint32_t mode_led(uint32_t li, uint32_t level, uint32_t now);
#define MODE_LED(li, level, now) mode_led(li, level, now)
/* Button i as a mode's toggle, hold or cycle button: 1 if it was. */
int mode_toggle_button(uint32_t i, int down);
/* Button i as a function button of the active mode: 1 if it was. */
int mode_function_button(uint32_t i, int down);
/* Button i released after a preset switch (its release is silent): a hold
 * mode still ends with its button. */
void mode_late_release(uint32_t i);
#if GM_MODE_ONE_SHOT
/* Button i pressed (the live one): a one-shot's layer took it, so it ends;
 * before the press goes to the modes, the gestures and its action. */
void mode_one_shot_press(uint32_t i);
#endif
void modes_init(void);                  /* no mode on, no cycle on a step (before the settings are loaded) */
#if GM_N_CYCLES
void cycles_start(void);                /* each cycle on its first enabled step */
#endif
#else
#define MODE_LED(li, level, now) (level)
#endif
/* Which of each base and its twins is live (components/modes/README.md "Button layers"),
 * then EVENT 8 for each mode that went off or on. */
#if GM_LAYER_PARAMS
void layers_update(void);
void layers_report_poll(void);          /* gm_poll: the EVENT 8 replay gm_layers_report asked for */
#else
#define layers_update() ((void)0)
#endif

/* ---- LEDs (components/leds) -------------------------------------------- */

/* LED levels (see led_show): LV_BLINK | level blinks it; LV_RGB | m lights
 * the pins in mask m (an RGB colour, or a cycle step's colour); LV_RGB |
 * LV_COLOUR | m shows led_colour[] (a DAW's colour, FB_LED_RGB) on an LED
 * that can show any colour (a full-colour hub's, VPIN_RGB), else mask m;
 * LV_RGB | LV_STEP(s) | m shows step s's full colour (gm_mode_rgb[s],
 * GM_MODE_RGB) there, else mask m. */
#define LV_BLINK 0x80u
#define LV_RGB 0x40u
#define LV_COLOUR 0x20u
#define LV_STEP_SHIFT 8u                /* mode index + 1, 7 bits (the builder's MODE_RGB_MAX) */
#define LV_STEP(m) (((m) + 1u) << LV_STEP_SHIFT)
#if GM_LAYERS
extern uint8_t led_dead[GM_ARR(GM_N_LEDS)];     /* modes (layers_update): a layer twin (or its base) not shown now */
void led_redraw(uint32_t i);            /* LED i is drawn again at the next leds_update */
#endif
#define LED_MS 10u                      /* gm_poll: leds_update every LED_MS */
void leds_init(void);
void leds_lamp(int on);                 /* the lamp test: every LED on (1), then off (0) and drawn again */
void leds_update(uint32_t now);         /* every LED_MS: what each LED shows */
int leds_apply(void);                   /* the passes between: show a few of them; 1 if it did any */
int leds_round_open(void);              /* a fade round is being shown: the main-loop hubs wait */

/* ---- host feedback (feedback.c) ---------------------------------------- */

#if GM_FEEDBACK
void feedback_init(void);               /* gm_init: which LED sources the device has */
#endif

/* ---- raw readings (monitor.c) ------------------------------------------ */

#if GM_MONITOR
void monitor_poll(uint32_t now);        /* gm_poll: the next RAW message, while a host asks for them */
#endif

/* ---- the scan (controller.c) ------------------------------------------- */

/* Scan ticks: +1 per gm_scan_tick, or per frame with the hardware scan
 * (which counts the time then). Read and written only at the scan's
 * interrupt level (the tick and the frames' interrupt share a priority, so
 * neither preempts the other): not volatile. A main-loop reader would need
 * it to be. */
extern uint32_t scan_ticks;
extern uint8_t hw_on;                   /* the scan: the platform scans; frames come to gm_scan_frames */
#if GM_HW_SCAN
extern uint32_t hw_frame_q4;            /* the scan: the hardware scan's frame period, 1/16 us */
#endif

/* ---- key matrix (components/key_matrix) -------------------------------- */

#if GM_HAS_MATRIX
#ifndef GM_MATRIX_ROWS
#define GM_MATRIX_ROWS 32               /* rows scanned: n_rows, at most 32 (a config generated before it) */
#endif
#define FAST_ROWS GM_ARR(GM_MATRIX_ROWS)    /* rows scanned (at most 32) */
extern uint32_t mprev[FAST_ROWS];       /* key matrix: each row's last reading; the raw readings */
#if GM_WRITE_LAST_PRESET
extern uint8_t keys_down;               /* key matrix: keys held (gm_idle_ms) */
#endif
void matrix_init(void);                 /* gm_init: its pins, and the scan's fast paths */
void matrix_scan(void);                 /* the scan tick, without the hardware scan: every row */
void kt0_age(void);                     /* the scan tick: one timing key's age */
#if GM_HW_SCAN
/* The key rows' part of the hardware scan's plan: 1 done (or no key
 * matrix), 0 the matrix can't be scanned so (logged), -1 its rows or
 * columns are on other registers than the plan's. */
int matrix_plan(gm_scan_plan_t *plan, uint32_t max_rows);
uint32_t matrix_retry(uint32_t key_rows);   /* a key change didn't fit: every key row of the frame again */
void matrix_frame(const uint16_t *raw, uint32_t chg, uint32_t key_rows);    /* a frame's changed key rows */
#endif
void keys_poll(void);                   /* the main loop: the queued key changes, as notes */
void keys_off(void);                    /* Note Off for every held key; their releases send nothing */
#endif

/* ---- analog inputs (components/analog) --------------------------------- */

#if GM_N_ANALOGS > 0
#define ANALOG_MS 2u                    /* one analog input per period (without the hardware analog scan) */
/* An input's channel and output range (channel:, min:, max:). */
#if GM_ANALOG_CTL
#define AN_CH(i) ctl_chan(&gm_analog_ctl[i])
#define AN_OUT(i, v, top) ctl_out(&gm_analog_ctl[i], (int32_t)(v), (top))
#else
#define AN_CH(i) midi_chan()
#define AN_OUT(i, v, top) ((uint32_t)(v))
#endif
#if GM_MONITOR
extern int16_t a_raw[NA];               /* analog: the last reading, -1 = none, RAW (sysex.md 4.8) */
#endif
#if GM_MSG_KINDS
uint32_t an_kind(int32_t mode);         /* what analog.mode sends: K_CC for cc, 0xFF for pitch bend and pressure */
#endif
void analog_init(void);
void analog_poll(void);
void analog_pickup_off(void);           /* every input back in control of its value (pick-up) */
#endif

/* ---- encoders (components/encoders) ------------------------------------ */

#if GM_N_ENCODERS > 0
/* An encoder's channel (channel:). */
#if GM_ENC_CTL
#define ENC_CH(i) ctl_chan(&gm_encoder_ctl[i])
#else
#define ENC_CH(i) midi_chan()
#endif
/* Detents, free running, per scanned encoder (twins have none): only
 * encoder_step writes it (the scan interrupt, or the main loop for I2C
 * hubs, never both for one encoder), and encoders_poll takes the turns as
 * the change since its last look. No read-modify-write from both sides, so
 * a detent counted between the main loop's load and store isn't lost (the
 * M0 has no LDREX; a halfword load or store is atomic). */
extern volatile uint16_t enc_count[GM_ARR(NE_HW)];   /* encoders; the raw readings read it */
extern uint8_t enc_i2c[GM_ARR(NE_HW)];  /* encoders (encoders_init): on an I2C hub, stepped from the main loop */
extern uint8_t enc_matrix[GM_ARR(NE_HW)]; /* encoders (encoders_init): both pins on switch matrices, stepped once they're read */
extern int16_t enc_value[NE];           /* encoders, and host feedback (enc_at): an absolute position */
#if GM_ENC_TWINS
extern uint8_t enc_live[GM_ARR(NE_HW)]; /* modes (layers_update): per scanned encoder, the index its turns go to */
#define ENC_LIVE(i) enc_live[i]
#else
#define ENC_LIVE(i) (i)
#endif
#if GM_MSG_KINDS
uint32_t enc_kind(int32_t mode);        /* what encoder.mode sends: K_CC for a 7-bit mode, else its K_* */
#endif
int32_t enc_top(uint32_t i);            /* the top of encoder i's values: 127, or 16383 in a 14-bit mode */
int32_t enc_span(uint32_t i, int32_t top);      /* an absolute mode's positions, 0..span; 0: relative */
uint32_t enc_out(uint32_t i, int32_t p, int32_t top);   /* the value position p sends */
void enc_at(uint32_t i, int32_t v, int32_t top, int32_t span);  /* go to the position of value v */
void encoder_step(uint32_t i);          /* the scan: read A and B, count a detent */
void encoders_init(void);
void encoders_poll(uint32_t now);
#endif

/* ---- pads (components/pads) -------------------------------------------- */

#if GM_N_PADS > 0
#define NP GM_N_PADS
#define PAD_MS 1u                       /* gm_poll reads the pads every PAD_MS */
enum { PAD_BLOCKED, PAD_IDLE, PAD_PEAK, PAD_ON };
/* A hit lights its pad's LED (active look) for at least this long: a quick
 * strike can start and end between two 10 ms LED updates, which left the
 * pad on its idle look. */
#define PAD_LED_MIN_MS 80u
extern uint8_t pad_state[NP];           /* pads: PAD_*; the LEDs show it */
extern uint32_t pad_lit[NP];            /* pads: ms of the last hit (| 1), 0 = none */
#if GM_PAD_TWINS
extern uint8_t pad_live[NP];            /* modes (layers_update): live twin + 1, 0 = the pad itself */
extern uint8_t pad_owner[NP];           /* pads: who took the last hit: twin + 1, 0 = the pad */
#define PAD_OWNER(i) pad_owner[i]
#else
#define PAD_OWNER(i) 0u
#endif
#if GM_MONITOR
extern uint16_t pad_raw[NP];            /* pads: the last reading, RAW (sysex.md 4.8) */
#endif
void pads_init(void);
void pads_scan(uint32_t now);
void pads_off(void);                    /* Note Off for every sounding pad (preset switch) */
void pads_report(void);                 /* at boot-log time: pads that read above pad.release */
#endif

#ifdef __cplusplus
}
#endif

#endif
