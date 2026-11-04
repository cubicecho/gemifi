/*
 * gm_din.h: 5-pin DIN MIDI ports (core/src/din.c, core/README.md "DIN
 * MIDI"). Only with din_midi: in the device YAML (GM_DIN). The platform
 * gives a 31250-baud UART (gm_hal.h hal_din_*); this routes it:
 *
 *   DIN In  -> DIN Out           midi.thru
 *   the controller -> DIN Out     midi.din_out
 *   DIN In  -> USB, USB -> DIN Out, by din_midi.mode:
 *     separate_port  a second USB-MIDI port (cable GM_DIN_CABLE) that is
 *                    a plain interface for the DIN jacks, both ways
 *     usb_merge      DIN In comes in on the main port; channel and
 *                    realtime messages from the host also go to DIN Out
 *                    when midi.usb_to_din is on
 *     din_only       no USB link
 *
 * DIN Out carries whole messages, with running status: a SysEx from one
 * source holds back the others' messages (realtime bytes still go straight
 * through, as MIDI allows) until its F7.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GM_DIN_H
#define GM_DIN_H

#include <stdint.h>
#include "gm_device_config.h"

#ifndef GM_DIN
#define GM_DIN 0
#endif

/* din_midi.mode (GM_DIN_MODE) */
#define GM_DIN_ONLY 0
#define GM_DIN_PORT 1
#define GM_DIN_MERGE 2

void gm_din_init(void);
void gm_din_poll(uint32_t now_ms);
/* A message the controller sends (keys, CCs, program changes). */
void gm_din_local(const uint8_t *m, uint32_t n);
/* A USB-MIDI event from the host. 1 = it was for the DIN port (separate_port,
 * cable GM_DIN_CABLE) and nothing else should see it. */
int gm_din_usb(const uint8_t ev[4]);

/* Counters: messages (or SysEx bytes) DIN Out had no room for, and DIN In
 * SysEx too long to merge onto the main USB port (usb_merge). */
extern uint32_t gm_din_out_drops, gm_din_sysex_drops;

#endif
