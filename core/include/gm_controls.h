/*
 * gm_controls.h: what the controller (core/src/controller.c) and the control
 * components (components/pads, ...) share: the helpers every control sends
 * with, the state one control keeps and another reads, and what the main
 * loop calls. Each shared variable is written by one component, named with
 * it. Not for the platforms (gm_controller.h is theirs).
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

#ifndef GM_PAD_TWINS
#define GM_PAD_TWINS 0                  /* a config generated before pad twins */
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

/* ---- MIDI out (controller.c) ---------------------------------------------- */

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

/* ---- MIDI messages (controller.c, core/README.md "MIDI messages") --------- */

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

/* ---- analog inputs (components/analog) ------------------------------------ */

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

/* ---- encoders (components/encoders) --------------------------------------- */

#if GM_N_ENCODERS > 0
/* An encoder's channel (channel:). */
#if GM_ENC_CTL
#define ENC_CH(i) ctl_chan(&gm_encoder_ctl[i])
#else
#define ENC_CH(i) midi_chan()
#endif
/* Detents, free running: only encoder_step writes it (the scan interrupt,
 * or the main loop for I2C hubs). */
extern volatile uint16_t enc_count[GM_ARR(NE_HW)];   /* encoders; the raw readings read it */
extern uint8_t enc_i2c[GM_ARR(NE_HW)];  /* encoders (encoders_init): on an I2C hub, stepped from the main loop */
extern int16_t enc_value[NE];           /* encoders, and host feedback (enc_at): an absolute position */
#if GM_ENC_TWINS
extern uint8_t enc_live[NE];            /* modes (layers_update): per scanned encoder, the index its turns go to */
#define ENC_LIVE(i) enc_live[i]
#else
#define ENC_LIVE(i) (i)
#endif
int32_t enc_top(uint32_t i);            /* the top of encoder i's values: 127, or 16383 in a 14-bit mode */
int32_t enc_span(uint32_t i, int32_t top);      /* an absolute mode's positions, 0..span; 0: relative */
uint32_t enc_out(uint32_t i, int32_t p, int32_t top);   /* the value position p sends */
void enc_at(uint32_t i, int32_t v, int32_t top, int32_t span);  /* go to the position of value v */
void encoder_step(uint32_t i);          /* the scan: read A and B, count a detent */
void encoders_init(void);
void encoders_poll(uint32_t now);
#endif

/* ---- pads (components/pads) ----------------------------------------------- */

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

#endif
