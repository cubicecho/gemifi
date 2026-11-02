/*
 * gm_feedback.h: host -> device feedback inside the core (core/README.md
 * "DAW feedback"). Internal: midi_in.c, controller.c and mcu.c talk through
 * this; platforms only call gm_midi_rx_byte / gm_midi_rx_usb.
 *
 * Compile-time switches come from gm_device_config.h (tools/builder):
 *   GM_FEEDBACK       F1: plain MIDI feedback on the main port (LEDs follow
 *                     notes/CCs, cc_toggle sync, encoder/analog values,
 *                     clock and transport LEDs)
 *   GM_LED_FB         per-LED table gm_led_fb[] (sources, owner, timeout)
 *   GM_LED_OWNER      default LED owner (GM_OWNER_*) without the table
 *   GM_LED_TIMEOUT    default host_then_local timeout, 10 ms units
 *   GM_ANALOG_PICKUP  gm_analog_pickup[] (analog takeover: pickup)
 *   GM_MCU            Mackie Control (gm_mcu.h, src/mcu.c)
 *   GM_MCU_CABLE      the cable MCU lives on: 1 = DAW port, 0 = the only port
 *   GM_USB_DAW_PORT   USB descriptor has a second cable "<name> DAW"
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_FEEDBACK_H
#define GEMIFI_GM_FEEDBACK_H

#include <stdint.h>
#include "gm_device_config.h"

#ifndef GM_FEEDBACK
#define GM_FEEDBACK 0
#endif
#ifndef GM_LED_FB
#define GM_LED_FB 0
#endif
#ifndef GM_LED_OWNER
#define GM_LED_OWNER 0              /* GM_OWNER_HOST_THEN_LOCAL */
#endif
#ifndef GM_LED_TIMEOUT
#define GM_LED_TIMEOUT 200          /* 2 s */
#endif
#ifndef GM_ANALOG_PICKUP
#define GM_ANALOG_PICKUP 0
#endif
#ifndef GM_MCU
#define GM_MCU 0
#endif
#ifndef GM_MCU_CABLE
#define GM_MCU_CABLE 0
#endif
#ifndef GM_USB_DAW_PORT
#define GM_USB_DAW_PORT 0
#endif

/* LEDs keep a host level when anything can drive them from the host. */
#define GM_HOST_LEDS (GM_FEEDBACK || GM_MCU)

#ifdef __cplusplus
extern "C" {
#endif

/* controller.c: one complete channel or realtime message from the host on
 * the main port (F1). m[0] is the status byte; n = 1..3. */
void gm_fb_msg(const uint8_t *m, uint32_t n);

/* controller.c: the host sets LED `led` to a MIDI value (velocity / CC
 * value): 0 = off, 1 = blinking, 2..127 = on (MCU's 0 / 1 / 127 palette). */
void gm_led_host(uint32_t led, uint32_t value);

/* controller.c: the host's value (0..127) of analog input i (pick-up). */
void gm_analog_host(uint32_t i, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
