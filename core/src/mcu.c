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
 *   characters       CC 0x40-0x4B, host -> device, one character each
 *   SysEx            F0 00 00 66 <id> <cmd> ... F7:
 *     00             device query -> 01 <serial 7> <challenge 4>
 *     02 <serial> <response 4>  -> 03 <serial> (connected; any response)
 *     12 <pos> <text...>        LCD text
 *     13 00          version request -> 14 <5 ASCII> ("V0.01" for 0.1.x)
 *     20 <strip> <mode>         meter mode of a strip; 21 <0/1> LCD meters
 *     61 / 62 / 63   faders to minimum / all LEDs off / reset
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_mcu.h"
#include "gm_feedback.h"

#if GM_MCU
#include "gm_device.h"
#include "gm_hal.h"
#include "gm_settings.h"
#include "app_proto.h"

#ifndef GM_MCU_ID
#define GM_MCU_ID MCU_SYSEX_ID_MCU
#endif

static uint8_t ring[8];
static uint8_t chars[MCU_N_CHARS];
static uint8_t meter_mode[8];
static uint8_t sx[8];               /* SysEx body (after F0), first bytes only */
static uint8_t sx_n;                /* body bytes so far; 0xFF = not in SysEx */
static uint8_t version[5];
static uint8_t lcd[8];              /* LCD text not yet passed to the hook */
static uint8_t lcd_n;
static uint8_t lcd_pos;             /* position of lcd[0] */

#define SX_HEAD 5u                  /* 00 00 66 <id> <cmd> */

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

uint8_t gm_mcu_char(uint32_t pos)
{
    return pos < MCU_N_CHARS ? chars[pos] : 0;
}

char gm_mcu_char_ascii(uint8_t v)
{
    uint32_t c = v & 0x3Fu;
    return (char)(c < 0x20u ? c + 0x40u : c);
}

uint8_t gm_mcu_meter_mode(uint32_t strip)
{
    return strip < 8 ? meter_mode[strip] : 0;
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
/* Show MCU ring value v (0 c mm pppp) on the encoder's ring: position 1..11
 * along the ring (0 = all off), spread uses positions 1..6 for its width. */
static void ring_show(uint32_t e, uint32_t v)
{
    int32_t pos = (int32_t)(v & 0x0Fu), mode = (int32_t)((v >> 4) & 3u);
    if (pos > 11)
        pos = 11;
    int32_t frac = -1;
    if (pos)
        frac = mode == 3 ? ((pos - 1) * 16383 / 5 > 16383 ? 16383 : (pos - 1) * 16383 / 5)
                         : (pos - 1) * 16383 / 10;
    gm_ring_draw(e, (uint32_t)mode, frac, (v & 0x40u) != 0);
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
        } else if (a >= MCU_CC_CHAR && a < MCU_CC_CHAR + MCU_N_CHARS) {
            chars[a - MCU_CC_CHAR] = (uint8_t)b;
            gm_mcu_on_char(a - MCU_CC_CHAR, (uint8_t)b);
        }
        break;
    case 0xD0:                              /* meters: strip << 4 | level */
        if (!ch)
            gm_mcu_on_meter(a >> 4, a & 0x0Fu);
        break;
    default:
        break;
    }
}

/* Every LED the host drives through MCU (button LEDs, MCU LEDs, rings) off. */
static void leds_off(void)
{
#if GM_N_BUTTONS > 0
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++)
        if (gm_cfg.btn_action[i] == ACT_MCU && gm_device.buttons[i].led >= 0)
            gm_led_host((uint32_t)gm_device.buttons[i].led, 0);
#endif
#if GM_LED_FB
    for (uint32_t i = 0; i < GM_N_LEDS; i++)
        if (gm_led_fb[i].source == GM_FB_MCU)
            gm_led_host(i, 0);
#endif
    for (uint32_t v = 0; v < 8; v++)
        ring[v] = 0;
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < GM_N_ENCODERS; i++)
        if (gm_encoder_mcu[i] != GM_MCU_NONE)
            ring_show(i, 0);
#endif
}

static void faders_min(void)
{
#if GM_N_ANALOGS > 0
    for (uint32_t i = 0; i < GM_N_ANALOGS; i++)
        if (gm_analog_mcu[i] != GM_MCU_NONE)
            gm_analog_host(i, 0);
#endif
}

/* F0 00 00 66 <id> <cmd> [serial] [extra] F7 */
static void reply(uint8_t cmd, const uint8_t *extra, uint32_t n)
{
    uint8_t r[6 + MCU_SERIAL_LEN + 4 + 1] = { 0xF0, 0x00, 0x00, 0x66, GM_MCU_ID, cmd };
    char hex[25];
    app_proto_uid_hex(hex);
    for (uint32_t i = 0; i < MCU_SERIAL_LEN; i++)   /* the last 7 hex digits of the chip UID */
        r[6 + i] = (uint8_t)hex[24 - MCU_SERIAL_LEN + i];
    for (uint32_t i = 0; i < n; i++)
        r[6 + MCU_SERIAL_LEN + i] = extra[i];
    r[6 + MCU_SERIAL_LEN + n] = 0xF7;
    send(r, 6 + MCU_SERIAL_LEN + n + 1);
}

static void lcd_flush(void)
{
    if (lcd_n) {
        gm_mcu_on_lcd(lcd_pos, lcd, lcd_n);
        lcd_pos = (uint8_t)(lcd_pos + lcd_n);
        lcd_n = 0;
    }
}

static int is_mcu(void)
{
    return sx[0] == 0 && sx[1] == 0 && sx[2] == 0x66 && sx[3] == GM_MCU_ID;
}

static void sysex_end(void)
{
    if (sx_n < SX_HEAD || !is_mcu())
        return;
    switch (sx[4]) {
    case 0x00: {                            /* device query: start the handshake */
        static const uint8_t challenge[4] = { 0x01, 0x02, 0x03, 0x04 };
        reply(0x01, challenge, sizeof challenge);
        break;
    }
    case 0x02:                              /* host's answer: accepted unchecked */
        reply(0x03, 0, 0);
        break;
    case 0x12:
        lcd_flush();
        break;
    case 0x13:
        if (sx_n == SX_HEAD + 1 && sx[5] == 0x00) {
            uint8_t r[12] = { 0xF0, 0x00, 0x00, 0x66, GM_MCU_ID, 0x14 };
            for (uint32_t i = 0; i < 5; i++)
                r[6 + i] = version[i];
            r[11] = 0xF7;
            send(r, sizeof r);
        }
        break;
    case 0x20:
        if (sx_n == SX_HEAD + 2 && sx[5] < 8)
            meter_mode[sx[5]] = sx[6];
        break;
    case 0x61:
        faders_min();
        break;
    case 0x62:
        leds_off();
        break;
    case 0x63:
        leds_off();
        faders_min();
        for (uint32_t i = 0; i < MCU_N_CHARS; i++)
            chars[i] = 0;
        for (uint32_t i = 0; i < 8; i++)
            meter_mode[i] = 0;
        break;
    default:                                /* 0F offline, 21 LCD meters, 0A-0E settings */
        break;
    }
}

/* SysEx on the MCU port. The first bytes are kept for the command; LCD text
 * (12) streams to the hook in small pieces, so no 112-byte buffer. */
void gm_mcu_sysex(uint8_t b)
{
    if (b == 0xF0) {
        sx_n = 0;
        lcd_n = 0;
        return;
    }
    if (sx_n == 0xFF)
        return;
    if (b == 0xF7) {
        sysex_end();
        sx_n = 0xFF;
        return;
    }
    if (sx_n > SX_HEAD && sx[4] == 0x12 && is_mcu()) {
        if (sx_n == SX_HEAD + 1)
            lcd_pos = sx[5];                /* the first text byte: position known */
        if ((uint32_t)lcd_pos + lcd_n < MCU_LCD_SIZE)
            lcd[lcd_n++] = b;
        if (lcd_n == sizeof lcd)
            lcd_flush();
        if (sx_n < 0xFE)
            sx_n++;
        return;
    }
    if (sx_n < sizeof sx)
        sx[sx_n] = b;
    if (sx_n < 0xFE)
        sx_n++;
}

__attribute__((weak)) void gm_mcu_on_lcd(uint32_t pos, const uint8_t *text, uint32_t n)
{
    (void)pos;
    (void)text;
    (void)n;
}

__attribute__((weak)) void gm_mcu_on_char(uint32_t pos, uint8_t v)
{
    (void)pos;
    (void)v;
}

__attribute__((weak)) void gm_mcu_on_meter(uint32_t strip, uint32_t level)
{
    (void)strip;
    (void)level;
}

#endif
