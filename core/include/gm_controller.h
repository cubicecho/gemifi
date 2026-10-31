/*
 * gm_controller.h: the generic MIDI controller runtime (core/src).
 *
 * The platform's main():
 *
 *     platform init (clocks, USB/serial MIDI, timer)
 *     gm_init(fw_version);
 *     start a periodic timer calling gm_scan_tick() every gm_device.scan_us
 *     for (;;) { platform polling (USB); gm_poll(); }
 *
 * gm_scan_tick() runs from the timer interrupt (matrix + encoders: timing
 * matters for velocity); everything else runs in gm_poll().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_CONTROLLER_H
#define GEMIFI_GM_CONTROLLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* safe_mode = boot combo held: ignore the stored profile (defaults only). */
void gm_init(uint32_t fw_version, int safe_mode);
void gm_poll(void);
void gm_scan_tick(void);
/* The scan period actually in use, in us (starts as gm_device.scan_us). A
 * platform that has to slow the scan down (the scan overran its period)
 * sets it, so key velocities stay right. */
extern volatile uint32_t gm_scan_period_us;

/* Platform -> core: every received MIDI byte. */
void gm_midi_rx_byte(uint8_t b);

/* Buttons held right now (bit = button index), for the boot combo check
 * before gm_init (reads the pins directly). */
uint32_t gm_buttons_now(void);

#ifdef __cplusplus
}
#endif

#endif
