/*
 * hello: first bring-up firmware for the ESP platform (no controller core).
 *
 * MIDI transport up; answers HELLO and the Universal Identity Request;
 * sends a LOG every second; BOOT button (GPIO0): LOG + EVENT + note 60
 * on/off; REBOOT; FW_* uploads into the other OTA slot (hal_fw_ops).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "gm_esp.h"
#include "gm_hal.h"
#include "gm_controller.h"
#include "gsx.h"
#include "esp_system.h"

#ifndef GM_FW_VERSION
#define GM_FW_VERSION 0x00000100u           /* 0.0.1 */
#endif
#define BTN_PIN 0

static uint8_t rx_buf[GSX_OVERHEAD + 4 + 300];   /* FW_DATA: 256 B packed */
static gsx_rx_t rx;
static uint8_t rx_ready;
static uint8_t tx_buf[64];
static uint16_t tx_len;
static uint32_t last_req;                   /* 0x10000 | seq << 8 | cmd */
static uint8_t uid[12];
static uint8_t unsol_seq;
static uint32_t reboot_at;
static uint8_t reboot_pending;
static const hal_fw_ops_t *fw;

static void send_log(uint8_t level, const char *s)
{
    uint8_t buf[GSX_OVERHEAD + 2 + 1 + 4 + 64];   /* + 2: F0 and F7 */
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_LOG, unsol_seq++ & 0x7F);
    gsx_put(&t, level);
    gsx_put_u(&t, hal_millis(), 4);
    for (uint32_t n = 0; s[n] && n < 64; n++)
        gsx_put(&t, (uint8_t)s[n]);
    hal_midi_send(buf, gsx_tx_end(&t));
}

static void send_event(uint8_t type, uint32_t index, uint32_t value)
{
    uint8_t buf[GSX_OVERHEAD + 2 + 1 + 2 + 2 + 4];   /* + 2: F0 and F7 */
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_EVENT, unsol_seq++ & 0x7F);
    gsx_put(&t, type);
    gsx_put_u(&t, index, 2);
    gsx_put_u(&t, value, 2);
    gsx_put_u(&t, hal_millis(), 4);
    hal_midi_send(buf, gsx_tx_end(&t));
}

static void identity_reply(void)
{
    uint32_t v = GM_FW_VERSION;
    const uint8_t msg[] = {
        0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x7D, 0x67, 0x6D,
        GM_BOARD_ID & 0x7F, (GM_BOARD_ID >> 7) & 0x7F,
        (uint8_t)((v >> 24) & 0x7F), (uint8_t)((v >> 16) & 0x7F),
        (uint8_t)(v & 0x7F), 0x00, 0xF7,     /* as core/src/app_proto.c */
    };
    hal_midi_send(msg, sizeof(msg));
}

static int dispatch(const gsx_frame_t *f, gsx_tx_t *t)
{
    const uint8_t *p = f->payload;
    switch (f->cmd) {
    case GSX_CMD_HELLO:
        if (f->len != 0)
            return GSX_BAD_LEN;
        gsx_put(t, 0);                      /* role: app (A/B uploads) */
        gsx_put_u(t, GM_BOARD_ID, 2);
        gsx_put_u(t, GM_FW_VERSION, 5);
        gsx_put(t, GSX_PROTO_VERSION);
        gsx_put_u(t, fw->max_chunk, 2);
        gsx_put_u(t, GSX_CAP_FW_UPLOAD | GSX_CAP_LOG | GSX_CAP_EVENT, 2);
        gsx_put_packed(t, uid, sizeof(uid));
        return GSX_OK;
    case GSX_CMD_REBOOT:
        if (f->len != 1)
            return GSX_BAD_LEN;
        if (p[0] > 1)
            return GSX_BAD_CMD;
        reboot_pending = 1;
        reboot_at = hal_millis() + 50;
        return GSX_OK;
    case GSX_CMD_FW_BEGIN: {
        if (f->len != 17)
            return GSX_BAD_LEN;
        int st = fw->begin(gsx_get_u(p, 5), gsx_get_u(p + 5, 5),
                           (uint16_t)gsx_get_u(p + 10, 2), gsx_get_u(p + 12, 5));
        if (st == GSX_OK)
            gsx_put_u(t, fw->max_chunk, 2);
        return st;
    }
    case GSX_CMD_FW_DATA: {
        if (f->len < 5)
            return GSX_BAD_LEN;
        uint32_t off = gsx_get_u(p, 4);
        uint8_t *data = (uint8_t *)p + 4;   /* unpack in place */
        int n = gsx_unpack(data, f->len - 4, data);
        if (n <= 0 || n > fw->max_chunk)
            return GSX_BAD_LEN;
        int st = fw->data(off, data, (uint32_t)n);
        if (st == GSX_OK)
            gsx_put_u(t, off, 4);
        return st;
    }
    case GSX_CMD_FW_END:
        return f->len ? GSX_BAD_LEN : fw->end();
    case GSX_CMD_FW_ABORT:
        if (f->len)
            return GSX_BAD_LEN;
        fw->abort();
        return GSX_OK;
    }
    return GSX_BAD_CMD;
}

static void handle_frame(void)
{
    if (rx.state == 1 && rx.n == 4 && rx_buf[0] == 0x7E && rx_buf[2] == 0x06 &&
        rx_buf[3] == 0x01) {
        identity_reply();
        hal_midi_flush(200);
        return;
    }
    gsx_frame_t f;
    int pst = gsx_parse(&rx, &f);
    if (pst == GSX_NOT_OURS)
        return;
    gsx_tx_t t;
    if (pst != GSX_OK) {
        gsx_tx_begin(&t, tx_buf, f.cmd | GSX_REPLY, f.seq);
        gsx_put(&t, (uint8_t)pst);
        hal_midi_send(tx_buf, gsx_tx_end(&t));
        last_req = 0;
        hal_midi_flush(200);
        return;
    }
    uint32_t req = 0x10000u | (uint32_t)f.seq << 8 | f.cmd;
    if (req != last_req) {                  /* a resend gets the same reply */
        gsx_tx_begin(&t, tx_buf, f.cmd | GSX_REPLY, f.seq);
        uint16_t at = t.n;
        gsx_put(&t, 0);
        int st = dispatch(&f, &t);
        if (st != GSX_OK)
            t.n = at + 1u;
        tx_buf[at] = (uint8_t)st;
        tx_len = gsx_tx_end(&t);
        last_req = (st == GSX_BUSY) ? 0 : req;
    }
    hal_midi_send(tx_buf, tx_len);
    hal_midi_flush(200);
}

/* The HAL calls this for every received MIDI byte. */
void gm_midi_rx_byte(uint8_t b)
{
    if (!rx_ready && gsx_rx_byte(&rx, b))
        rx_ready = 1;                       /* handled from the main loop */
}

static void loop_once(void)
{
    static uint32_t next_log = 1000, next_btn;
    static uint8_t btn;
    gm_esp_loop();
    if (rx_ready) {
        handle_frame();
        gsx_rx_reset(&rx);
        rx_ready = 0;
    }
    uint32_t now = hal_millis();
    if ((int32_t)(now - next_log) >= 0) {
        char s[40];
        snprintf(s, sizeof(s), "alive %lu s, heap %lu", (unsigned long)(now / 1000),
                 (unsigned long)esp_get_free_heap_size());
        send_log(2, s);
        next_log += 1000;
    }
    if ((int32_t)(now - next_btn) >= 0) {   /* 10 ms sampling = debounce */
        next_btn = now + 10;
        uint8_t b = hal_pin_read(BTN_PIN) == 0;
        if (b != btn) {
            btn = b;
            const uint8_t note[3] = { (uint8_t)(b ? 0x90 : 0x80), 60, (uint8_t)(b ? 100 : 0) };
            hal_midi_send(note, 3);
            send_event(1, 0, b);
            send_log(2, b ? "BOOT button down" : "BOOT button up");
        }
    }
    if (reboot_pending && (int32_t)(now - reboot_at) >= 0)
        hal_reboot(0);
}

void app_main(void)
{
    rx.buf = rx_buf;
    rx.cap = sizeof(rx_buf);
    gsx_rx_reset(&rx);
    hal_uid(uid);
    fw = hal_fw_ops();
    hal_pin_mode(BTN_PIN, HAL_IN_PULLUP);
    gm_esp_begin();
    send_log(2, "hello: " GM_DEVICE_NAME);
    for (;;)
        loop_once();
}
