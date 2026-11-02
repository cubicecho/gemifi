/*
 * mcu.c: Mackie Control (MCU) on the DAW port. See gm_mcu.h; the whole file
 * is empty unless the device YAML has `mcu:` (GM_MCU).
 *
 * Message formats (community documentation of the MCU / Logic Control
 * protocol; nothing copied from any implementation):
 *   buttons / LEDs   Note On ch 1, note = button; device: 7F press, 00
 *                    release; host: 00 off, 01 blink, 7F on
 *   faders 1-8, master  pitch bend ch 1-8 / 9, 14 bit, both directions
 *   V-Pots           CC 0x10-0x17, device -> host, 01-0F = +n, 41-4F = -n
 *   V-Pot rings      CC 0x30-0x37, host -> device, value 0 c mm pppp
 *   meters           channel pressure ch 1, data = strip << 4 | level
 *   version          host F0 00 00 66 <id> 13 00 F7,
 *                    device F0 00 00 66 <id> 14 <5 ASCII> F7
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_mcu.h"
#include "gm_feedback.h"

#if GM_MCU
#include "gm_device.h"
#include "gm_hal.h"
#include "gm_settings.h"

#ifndef GM_MCU_ID
#define GM_MCU_ID MCU_SYSEX_ID_MCU
#endif

static uint8_t ring[8];
static uint8_t sx[8];               /* SysEx body (after F0), first bytes only */
static uint8_t sx_n;
static uint8_t version[5];

static void send(const uint8_t *m, uint32_t n)
{
    hal_midi_send_cable(GM_MCU_CABLE, m, n);
}

static void send3(uint32_t st, uint32_t a, uint32_t b)
{
    uint8_t m[3] = { (uint8_t)st, (uint8_t)(a & 0x7F), (uint8_t)(b & 0x7F) };
    send(m, 3);
}

void gm_mcu_init(uint32_t fw_version)
{
    /* "V" + major.minor as "1.02": the version string the host shows */
    uint32_t maj = (fw_version >> 24) % 10u, min = (fw_version >> 16) & 0xFFu;
    version[0] = 'V';
    version[1] = (uint8_t)('0' + maj);
    version[2] = '.';
    version[3] = (uint8_t)('0' + (min / 10u) % 10u);
    version[4] = (uint8_t)('0' + min % 10u);
    sx_n = 0xFF;
}

uint8_t gm_mcu_ring(uint32_t vpot)
{
    return vpot < 8 ? ring[vpot] : 0;
}

/* ---- device -> host ---------------------------------------------------- */

void gm_mcu_note(uint32_t note, int down)
{
    send3(0x90, note, down ? 0x7F : 0x00);
}

void gm_mcu_fader(uint32_t fader, uint32_t v14)
{
    if (v14 > 0x3FFF)
        v14 = 0x3FFF;
    send3(0xE0 | (fader & 0x0Fu), v14 & 0x7F, v14 >> 7);
}

void gm_mcu_vpot(uint32_t vpot, int32_t delta)
{
    if (!delta)
        return;
    uint32_t mag = (uint32_t)(delta < 0 ? -delta : delta);
    if (mag > 15)
        mag = 15;
    send3(0xB0, MCU_CC_VPOT + (vpot & 7u), (delta < 0 ? 0x40u : 0) | mag);
}

/* ---- host -> device ----------------------------------------------------- */

#if GM_N_ENCODERS > 0
/* Show ring value v on the encoder's ring LEDs. Position 1..11 is spread
 * over however many LEDs the ring has. Modes: 0 dot, 1 boost/cut (from the
 * centre), 2 wrap (fill from the left), 3 spread (both ways from the
 * centre). The centre LED bit (6) is not shown (TODO: a ring_center LED). */
static void ring_show(uint32_t e, uint32_t v)
{
    const gm_ring_t *r = &gm_encoder_ring[e];
    int32_t n = r->n, pos = (int32_t)(v & 0x0Fu), mode = (int32_t)((v >> 4) & 3u);
    if (!n)
        return;
    if (pos > 11)
        pos = 11;
    int32_t p = pos ? (pos - 1) * (n - 1) / 10 : -1;  /* LED index of the position */
    int32_t c = (n - 1) / 2;
    for (int32_t k = 0; k < n; k++) {
        int on = 0;
        if (pos) {
            switch (mode) {
            case 0:
                on = k == p;
                break;
            case 1:
                on = (k >= c && k <= p) || (k <= c && k >= p);
                break;
            case 2:
                on = k <= p;
                break;
            default: {
                int32_t w = (pos - 1) * (n / 2) / 5;    /* pos 1..6 = width 0..n/2 */
                on = (k >= c - w && k <= c + w);
                break;
            }
            }
        }
        gm_led_host(r->leds[k], on ? 127 : 0);
    }
}
#endif

void gm_mcu_msg(const uint8_t *m, uint32_t n)
{
    uint32_t type = m[0] & 0xF0u, ch = m[0] & 0x0Fu;
    if (n < 2 || m[0] >= 0xF0)
        return;                             /* realtime: MCU doesn't use it */
    uint32_t a = m[1], b = n > 2 ? m[2] : 0;
    switch (type) {
    case 0x80:
        b = 0;
        /* fall through */
    case 0x90:                              /* LED of MCU note a */
        if (ch)
            break;
#if GM_N_BUTTONS > 0
        for (uint32_t i = 0; i < GM_N_BUTTONS; i++)
            if (gm_cfg.btn_action[i] == ACT_MCU && (uint32_t)gm_cfg.btn_number[i] == a &&
                gm_device.buttons[i].led >= 0)
                gm_led_host((uint32_t)gm_device.buttons[i].led, b);
#endif
#if GM_LED_FB
        for (uint32_t i = 0; i < GM_N_LEDS; i++)
            if (gm_led_fb[i].source == GM_FB_MCU && gm_led_fb[i].num == a)
                gm_led_host(i, b);
#endif
        break;
    case 0xE0:                              /* fader position: pick-up target */
#if GM_N_ANALOGS > 0
        for (uint32_t i = 0; i < GM_N_ANALOGS; i++)
            if (gm_analog_mcu[i] == ch)
                gm_analog_host(i, b);       /* the top 7 of the 14 bits */
#endif
        break;
    case 0xB0:
        if (ch)
            break;
        if (a >= MCU_CC_RING && a < MCU_CC_RING + 8u) {
            ring[a - MCU_CC_RING] = (uint8_t)b;
#if GM_N_ENCODERS > 0
            for (uint32_t i = 0; i < GM_N_ENCODERS; i++)
                if (gm_encoder_mcu[i] == a - MCU_CC_RING)
                    ring_show(i, b);
#endif
        }
        /* TODO: CC 0x40-0x4B timecode / assignment display characters */
        break;
    case 0xD0:                              /* meters: strip << 4 | level */
        if (!ch)
            gm_mcu_on_meter(a >> 4, a & 0x0Fu);
        break;
    default:
        break;
    }
}

/* SysEx on the MCU port: F0 00 00 66 <id> <cmd> ... F7. Only the first
 * bytes are kept; the version request is the only one answered.
 * TODO: 00 device query (the host-connection challenge/response; Live
 * doesn't use it), 12 LCD text (-> gm_mcu_on_lcd), 20/21 meter modes. */
void gm_mcu_sysex(uint8_t b)
{
    if (b == 0xF0) {
        sx_n = 0;
        return;
    }
    if (b == 0xF7) {
        if (sx_n >= 5 && sx[0] == 0 && sx[1] == 0 && sx[2] == 0x66 && sx[3] == GM_MCU_ID &&
            sx[4] == 0x13) {
            uint8_t r[12] = { 0xF0, 0x00, 0x00, 0x66, GM_MCU_ID, 0x14 };
            for (uint32_t i = 0; i < 5; i++)
                r[6 + i] = version[i];
            r[11] = 0xF7;
            send(r, sizeof r);
        }
        sx_n = 0xFF;
        return;
    }
    if (sx_n < sizeof sx)
        sx[sx_n++] = b;
}

__attribute__((weak)) void gm_mcu_on_lcd(uint32_t pos, const uint8_t *text, uint32_t n)
{
    (void)pos;
    (void)text;
    (void)n;
}

__attribute__((weak)) void gm_mcu_on_meter(uint32_t strip, uint32_t level)
{
    (void)strip;
    (void)level;
}

#endif
