/*
 * midi_in.c: the incoming MIDI dispatcher. Platforms hand over what they
 * receive (gm_controller.h): a byte stream (serial, BLE) or USB-MIDI event
 * packets (NUC USBD, TinyUSB). This sorts it by port and kind:
 *
 *   SysEx, main port      -> app_proto_rx_byte (the gemifi protocol, as before)
 *   SysEx, MCU port       -> gm_mcu_sysex (version request)
 *   channel / realtime,
 *     main port           -> gm_fb_msg (F1 feedback, controller.c)
 *     MCU port            -> gm_mcu_msg
 *
 * "Main port" = cable 0. The MCU port is cable 1 when the device has a USB
 * DAW port, else cable 0 (MCU-only devices on serial / BLE). Without a DAW
 * port every cable counts as the main port for SysEx, which is what the
 * firmware did before there were cables.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controller.h"
#include "gm_feedback.h"
#include "gm_mcu.h"
#include "app_proto.h"

#define GM_LISTEN (GM_FEEDBACK || GM_MCU)

#if GM_LISTEN
static void msg(uint32_t cable, const uint8_t *m, uint32_t n)
{
#if GM_MCU
    if (cable == GM_MCU_CABLE) {
        gm_mcu_msg(m, n);
        return;
    }
#endif
#if GM_FEEDBACK
    if (cable == 0)
        gm_fb_msg(m, n);
#endif
}
#endif

/* USB-MIDI 1.0 (section 4): byte 0 = cable << 4 | CIN. CIN 4 = SysEx starts
 * or continues (3 bytes), 5/6/7 = SysEx ends with 1/2/3 bytes, 2/3 =
 * two/three-byte system common, 8..E = channel messages, F = one byte. */
void gm_midi_rx_usb(const uint8_t ev[4])
{
    uint32_t cable = ev[0] >> 4, cin = ev[0] & 0x0Fu;
    if (cin >= 4 && cin <= 7) {
        uint32_t k = (cin == 5) ? 1 : (cin == 6) ? 2 : 3;
        for (uint32_t i = 0; i < k; i++) {
            if (cable == 0 || !GM_USB_DAW_PORT)
                app_proto_rx_byte(ev[1 + i]);
#if GM_MCU
            if (cable == GM_MCU_CABLE)
                gm_mcu_sysex(ev[1 + i]);
#endif
        }
        return;
    }
#if GM_LISTEN
    /* message length per CIN; 0 = not a message we take (CIN 0/1 reserved) */
    static const uint8_t len[16] = { 0, 0, 2, 3, 0, 0, 0, 0, 3, 3, 3, 3, 2, 2, 3, 1 };
    uint32_t n = len[cin];
    if (n && (cin != 0x0F || ev[1] >= 0xF8))     /* CIN F: realtime only */
        msg(cable, ev + 1, n);
#endif
}

/* A MIDI 1.0 byte stream (one port). Every byte still goes to the SysEx
 * parser, as before; channel messages (with running status) and realtime
 * bytes are cut out of it for the feedback code. */
void gm_midi_rx_byte(uint8_t b)
{
    app_proto_rx_byte(b);
#if GM_LISTEN
    static uint8_t m[3], n, need, sx;
    if (b >= 0xF8) {                    /* realtime: anywhere, even in SysEx */
        msg(0, &b, 1);
        return;
    }
    if (b & 0x80) {
#if GM_MCU && GM_MCU_CABLE == 0
        if (b == 0xF0 || (sx && b == 0xF7))
            gm_mcu_sysex(b);
#endif
        sx = b == 0xF0;
        n = 0;
        need = 0;
        if (b < 0xF0) {                 /* channel status: becomes running status */
            m[0] = b;
            n = 1;
            need = (b & 0xE0) == 0xC0 ? 2 : 3;
        }
        return;                         /* system common: ignored, clears running status */
    }
    if (sx) {
#if GM_MCU && GM_MCU_CABLE == 0
        gm_mcu_sysex(b);
#endif
        return;
    }
    if (!need)
        return;
    if (n == 0)
        n = 1;                          /* running status: m[0] is still the status */
    m[n++] = b;
    if (n == need) {
        msg(0, m, n);
        n = 0;
    }
#endif
}
