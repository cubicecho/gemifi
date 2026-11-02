/*
 * gm_mcu.h: Mackie Control (MCU) on the DAW port (src/mcu.c). Compiled in
 * only when the device YAML has `mcu:` (GM_MCU = 1); otherwise mcu.c is
 * empty and nothing calls it.
 *
 * Written from community documentation of the MCU wire protocol (message
 * formats only; core/README.md "Mackie Control"). Implemented: buttons and
 * their LEDs (Note On ch 1), faders (pitch bend ch 1-9, 14 bit), V-Pots
 * (CC 0x10-0x17 relative out, ring CC 0x30-0x37 in), transport, and the
 * firmware version reply Ableton Live asks for. Not implemented (TODO):
 * the host-connection challenge/response, LCD text, meters, timecode
 * display, fader touch.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_MCU_H
#define GEMIFI_GM_MCU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCU_SYSEX_ID_MCU 0x14       /* device id in F0 00 00 66 <id> ... */
#define MCU_SYSEX_ID_XT  0x15
#define MCU_CC_VPOT      0x10       /* + V-Pot 0..7: rotation, device -> host */
#define MCU_CC_RING      0x30       /* + V-Pot 0..7: LED ring, host -> device */

void gm_mcu_init(uint32_t fw_version);

/* Host -> device on the MCU cable: one channel message, and SysEx bytes
 * (F0 .. F7 inclusive). */
void gm_mcu_msg(const uint8_t *m, uint32_t n);
void gm_mcu_sysex(uint8_t b);

/* Device -> host. */
void gm_mcu_note(uint32_t note, int down);           /* button press / release */
void gm_mcu_fader(uint32_t fader, uint32_t v14);     /* 0..7, 8 = master */
void gm_mcu_vpot(uint32_t vpot, int32_t delta);      /* detents, +cw */

/* The last ring value the host sent for V-Pot 0..7 (CC 0x30 + n value:
 * bit 6 = centre LED, bits 4-5 = mode, bits 0-3 = position 0..11). */
uint8_t gm_mcu_ring(uint32_t vpot);

/* Optional hooks (weak, do nothing) for devices with a display or meters.
 * TODO: nothing calls them yet except as noted in mcu.c. */
void gm_mcu_on_lcd(uint32_t pos, const uint8_t *text, uint32_t n);
void gm_mcu_on_meter(uint32_t strip, uint32_t level);

#ifdef __cplusplus
}
#endif

#endif
