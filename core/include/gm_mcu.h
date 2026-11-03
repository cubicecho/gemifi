/*
 * gm_mcu.h: Mackie Control (MCU) on the DAW port (src/mcu.c). Compiled in
 * only when the device YAML has `mcu:` (GM_MCU = 1); otherwise mcu.c is
 * empty and nothing calls it.
 *
 * Written from community documentation of the MCU wire protocol (message
 * formats only; core/README.md "Mackie Control"). Implemented: buttons and
 * their LEDs (Note On ch 1), faders (pitch bend ch 1-9, 14 bit), V-Pots
 * (CC 0x10-0x17 relative out, ring CC 0x30-0x37 in), transport, and the
 * firmware version reply Ableton Live asks for, the host-connection
 * handshake (any answer to the challenge is accepted: no response check),
 * LCD text, the 12 timecode / assignment characters, meter levels and
 * modes (to hooks: a device with a display or meter LEDs shows them), and
 * the "all LEDs off", "faders to minimum" and reset commands.
 * Not implemented: fader touch, extender chaining (#18).
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
#define MCU_CC_CHAR      0x40       /* + 0..11: timecode / assignment character */
#define MCU_N_CHARS      12u
#define MCU_LCD_SIZE     112u       /* 2 rows of 56 characters */
#define MCU_SERIAL_LEN   7u

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

/* The last character the host sent for display position 0..11 (CC 0x40 + n:
 * bit 6 = decimal point, bits 0-5 = the character), and that character as
 * ASCII (MCU codes 0x00-0x1F are '@'..'_', 0x20-0x3F are ' '..'?'). */
uint8_t gm_mcu_char(uint32_t pos);
char gm_mcu_char_ascii(uint8_t v);

/* The meter mode the host set for strip 0..7 (SysEx 20: bit 0 signal LED,
 * bit 1 peak hold, bit 2 LCD meter). */
uint8_t gm_mcu_meter_mode(uint32_t strip);

/* Optional hooks (weak, do nothing) for devices with a display or meters.
 *   on_lcd   LCD text (SysEx 12): n characters from position pos (0..111,
 *            56 per row); long updates arrive in several calls
 *   on_char  CC 0x40 + pos: v as for gm_mcu_char()
 *   on_meter channel pressure: strip 0..7, level 0..12, 14 = overload set,
 *            15 = overload clear. The device decays meters itself (about
 *            one step per 300 ms) */
void gm_mcu_on_lcd(uint32_t pos, const uint8_t *text, uint32_t n);
void gm_mcu_on_char(uint32_t pos, uint8_t v);
void gm_mcu_on_meter(uint32_t strip, uint32_t level);

#ifdef __cplusplus
}
#endif

#endif
