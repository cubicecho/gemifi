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

#ifndef GM_PAD_TWINS
#define GM_PAD_TWINS 0                  /* a config generated before pad twins */
#endif

/* ---- MIDI out (controller.c) ---------------------------------------------- */

uint8_t midi_chan(void);                /* the device's MIDI channel, 0-15 (midi.channel - 1) */
void midi3(uint8_t status, uint32_t a, uint32_t b);
void send_event(uint8_t type, uint32_t index, uint32_t value);   /* an EVENT, with debug.events on */

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
