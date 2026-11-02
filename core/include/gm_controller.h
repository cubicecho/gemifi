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

/* Platform -> core: received MIDI (core/src/midi_in.c). A platform calls
 * one of the two for everything it receives:
 *   gm_midi_rx_byte  a MIDI 1.0 byte stream (serial, BLE): one port, cable 0;
 *   gm_midi_rx_usb   one 4-byte USB-MIDI 1.0 event packet (cable << 4 | CIN,
 *                    then 3 bytes), so the cable (port) is kept.
 * SysEx on the main port goes to the gemifi protocol (app_proto.h) as it
 * always did; channel messages and realtime (clock, start/stop) go to the
 * host feedback code (F1) and, on the DAW port, to Mackie Control (gm_mcu.h)
 * when the device has them (core/README.md "DAW feedback"). */
void gm_midi_rx_byte(uint8_t b);
void gm_midi_rx_usb(const uint8_t ev[4]);

/* Buttons held right now (bit = button index), for the boot combo check
 * before gm_init (reads the pins directly). */
uint32_t gm_buttons_now(void);

#ifdef __cplusplus
}
#endif

#endif
