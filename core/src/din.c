/*
 * din.c: the 5-pin DIN MIDI ports (gm_din.h, core/README.md "DIN MIDI").
 *
 * DIN In is parsed into messages (running status, SysEx, realtime, system
 * common) and each goes to DIN Out (midi.thru) and to USB (din_midi.mode).
 * DIN Out takes whole messages from three sources: the controller, the thru
 * and the host. While one source's SysEx is going out, the others' messages
 * wait in a short queue (realtime goes straight through); a SysEx from a
 * second source meanwhile is dropped, and one that stalls for SX_TIMEOUT_MS
 * is ended with F7.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_din.h"
#include "gm_hal.h"
#include "gm_settings.h"
#include "app_proto.h"

/* Platforms without DIN MIDI (gm_hal.h). */
__attribute__((weak)) int hal_din_open(uint16_t tx_pin, uint16_t rx_pin, uint32_t baud, uint32_t flags)
{
    (void)tx_pin;
    (void)rx_pin;
    (void)baud;
    (void)flags;
    return 0;
}

__attribute__((weak)) int hal_din_write(const uint8_t *b, uint32_t n)
{
    (void)b;
    (void)n;
    return 0;
}

__attribute__((weak)) int hal_din_read(void)
{
    return -1;
}

__attribute__((weak)) void hal_midi_send_usb(const uint8_t ev[4])
{
    (void)ev;
}

uint32_t gm_din_out_drops, gm_din_sysex_drops;

#if GM_DIN

enum { SRC_LOCAL, SRC_THRU, SRC_USB, N_SRC, SRC_NONE = 0xFF };

#define PEND_MAX 16u                /* messages held back by another source's SysEx */
#define SX_TIMEOUT_MS 500u
#define RX_PER_POLL 64u             /* DIN In bytes handled per gm_din_poll */

static uint32_t now_ms;

/* ---- DIN Out ----------------------------------------------------------- */

static uint8_t sx_owner = SRC_NONE; /* the source whose SysEx is going out */
static uint8_t sx_skip[N_SRC];      /* this source's SysEx is being dropped */
static uint32_t sx_t;               /* ms of the owner's last SysEx byte */
static uint8_t pend[PEND_MAX][4];   /* [0] = length, then the message */
static uint8_t pend_n;

static int out(const uint8_t *b, uint32_t n)
{
    if (hal_din_write(b, n))
        return 1;
    gm_din_out_drops++;
    return 0;
}

static void sysex_done(void)
{
    sx_owner = SRC_NONE;
    for (uint32_t i = 0; i < pend_n; i++)
        out(pend[i] + 1, pend[i][0]);
    pend_n = 0;
}

static void send_short(uint8_t src, const uint8_t *m, uint32_t n)
{
    if (n == 0 || n > 3)
        return;
    if (m[0] < 0xF8 && sx_owner == src) {
        static const uint8_t f7 = 0xF7;     /* a status byte ends its own SysEx */
        out(&f7, 1);
        sysex_done();
    }
    if (m[0] >= 0xF8 || sx_owner == SRC_NONE) {
        out(m, n);
        return;
    }
    if (pend_n >= PEND_MAX) {
        gm_din_out_drops++;
        return;
    }
    pend[pend_n][0] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++)
        pend[pend_n][1 + i] = m[i];
    pend_n++;
}

static void send_sysex(uint8_t src, uint8_t b)
{
    if (b == 0xF0) {
        if (sx_owner != SRC_NONE && sx_owner != src) {
            sx_skip[src] = 1;               /* another source's SysEx is going out */
            gm_din_out_drops++;
            return;
        }
        sx_skip[src] = 0;
        sx_owner = src;
        sx_t = now_ms;
        if (!out(&b, 1)) {
            sx_skip[src] = 1;
            sysex_done();
        }
        return;
    }
    if (sx_skip[src]) {
        if (b == 0xF7)
            sx_skip[src] = 0;
        return;
    }
    if (sx_owner != src)
        return;                             /* no F0 seen */
    sx_t = now_ms;
    if (!out(&b, 1)) {
        /* No room: cut it short. The F7 may not fit either; the next
         * status byte ends it for the receiver then. */
        static const uint8_t f7 = 0xF7;
        sx_skip[src] = b != 0xF7;
        (void)hal_din_write(&f7, 1);
        sysex_done();
        return;
    }
    if (b == 0xF7)
        sysex_done();
}

/* ---- to USB ------------------------------------------------------------ */

#if GM_DIN_MODE != GM_DIN_ONLY
#if GM_DIN_MODE == GM_DIN_PORT
#define USB_CABLE GM_DIN_CABLE
#else
#define USB_CABLE 0u
#endif

static void usb_event(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2)
{
    const uint8_t ev[4] = { (uint8_t)(USB_CABLE << 4 | cin), b0, b1, b2 };
    hal_midi_send_usb(ev);
}

/* A short message as one USB-MIDI event (USB-MIDI 1.0 table 4-1). */
static void usb_short(const uint8_t *m, uint32_t n)
{
    uint8_t s = m[0], cin;
    if (s < 0xF0)
        cin = s >> 4;
    else if (s >= 0xF8)
        cin = 0x0F;
    else if (s == 0xF2)
        cin = 3;
    else if (s == 0xF6)
        cin = 5;
    else
        cin = 2;                            /* F1, F3 */
    usb_event(cin, s, n > 1 ? m[1] : 0, n > 2 ? m[2] : 0);
}

#if GM_DIN_MODE == GM_DIN_PORT
/* Its own cable: SysEx streams out three bytes per event as it comes. */
static uint8_t usx[3], usx_n;

static void usb_sysex(uint8_t b)
{
    if (b == 0xF0)
        usx_n = 0;
    usx[usx_n++] = b;
    if (b == 0xF7) {
        usb_event((uint8_t)(4u + usx_n), usx[0], usx_n > 1 ? usx[1] : 0, usx_n > 2 ? usx[2] : 0);
        usx_n = 0;
    } else if (usx_n == 3) {
        usb_event(4, usx[0], usx[1], usx[2]);
        usx_n = 0;
    }
}
#else
/* The main port also carries the controller and the gemifi protocol, so a
 * SysEx goes out whole, in one go, or not at all. */
#define MERGE_SYSEX_MAX 256u
static uint8_t msx[MERGE_SYSEX_MAX];
static uint32_t msx_n;
static uint8_t msx_over;

static void usb_sysex(uint8_t b)
{
    if (b == 0xF0) {
        msx_n = 0;
        msx_over = 0;
    }
    if (msx_over)
        return;
    if (msx_n >= MERGE_SYSEX_MAX) {
        msx_over = 1;
        gm_din_sysex_drops++;
        return;
    }
    msx[msx_n++] = b;
    if (b != 0xF7)
        return;
    for (uint32_t i = 0; i < msx_n; i += 3) {
        uint32_t k = msx_n - i > 3 ? 3 : msx_n - i;
        uint8_t cin = msx_n - i > 3 ? 4 : (uint8_t)(4u + k);
        usb_event(cin, msx[i], k > 1 ? msx[i + 1] : 0, k > 2 ? msx[i + 2] : 0);
    }
    msx_n = 0;
}
#endif
#endif /* GM_DIN_MODE != GM_DIN_ONLY */

/* ---- DIN In ------------------------------------------------------------ */

static void in_msg(const uint8_t *m, uint32_t n)
{
    if (gm_cfg.din_thru)
        send_short(SRC_THRU, m, n);
#if GM_DIN_MODE != GM_DIN_ONLY
    usb_short(m, n);
#endif
}

static void in_sysex(uint8_t b)
{
    if (gm_cfg.din_thru)
        send_sysex(SRC_THRU, b);
#if GM_DIN_MODE != GM_DIN_ONLY
    usb_sysex(b);
#endif
}

static uint8_t in_m[3], in_n, in_need, in_sx;

static void parse(uint8_t b)
{
    if (b >= 0xF8) {                    /* realtime: anywhere, even inside SysEx */
        in_msg(&b, 1);
        return;
    }
    if (b & 0x80) {
        if (in_sx) {                    /* any status byte ends a SysEx */
            in_sx = 0;
            in_sysex(0xF7);
            if (b == 0xF7)
                return;
        }
        if (b == 0xF7)
            return;                     /* stray */
        in_n = 0;
        in_need = 0;
        if (b == 0xF0) {
            in_sx = 1;
            in_sysex(b);
            return;
        }
        in_m[0] = b;
        in_n = 1;
        if (b < 0xF0)                   /* channel: becomes running status */
            in_need = (b & 0xE0) == 0xC0 ? 2 : 3;
        else if (b == 0xF6)
            in_msg(in_m, 1);
        else if (b == 0xF2)
            in_need = 3;
        else if (b == 0xF1 || b == 0xF3)
            in_need = 2;                /* F4 / F5: undefined, ignored */
        return;
    }
    if (in_sx) {
        in_sysex(b);
        return;
    }
    if (!in_need)
        return;
    if (in_n == 0)
        in_n = 1;                       /* running status: in_m[0] is still the status */
    in_m[in_n++] = b;
    if (in_n == in_need) {
        in_msg(in_m, in_n);
        in_n = 0;
        if (in_m[0] >= 0xF0)
            in_need = 0;                /* system common has no running status */
    }
}

/* ---- the core's side --------------------------------------------------- */

void gm_din_init(void)
{
    sx_owner = SRC_NONE;
    pend_n = 0;
    in_n = in_need = in_sx = 0;
#ifndef GM_DIN_DMA
#define GM_DIN_DMA 0
#endif
    if (!hal_din_open(GM_DIN_TX_PIN, GM_DIN_RX_PIN, 31250u, GM_DIN_DMA ? HAL_UART_DMA : 0u))
        app_log(LOG_WARN, "DIN MIDI: the platform has no UART for din_midi.tx_pin", "");
}

void gm_din_poll(uint32_t now)
{
    now_ms = now;
    for (uint32_t i = 0; i < RX_PER_POLL; i++) {
        int b = hal_din_read();
        if (b < 0)
            break;
        parse((uint8_t)b);
    }
    if (sx_owner != SRC_NONE && now - sx_t > SX_TIMEOUT_MS) {
        static const uint8_t f7 = 0xF7;
        sx_skip[sx_owner] = 1;              /* the rest of it, if it ever comes */
        out(&f7, 1);
        sysex_done();
    }
}

void gm_din_local(const uint8_t *m, uint32_t n)
{
    if (gm_cfg.din_out)
        send_short(SRC_LOCAL, m, n);
}

int gm_din_usb(const uint8_t ev[4])
{
    static const uint8_t len[16] = { 0, 0, 2, 3, 0, 0, 0, 0, 3, 3, 3, 3, 2, 2, 3, 1 };
    uint32_t cable = ev[0] >> 4, cin = ev[0] & 0x0Fu;
    int sysex = cin >= 4 && cin <= 7 && !(cin == 5 && ev[1] == 0xF6);
#if GM_DIN_MODE == GM_DIN_PORT
    if (cable != GM_DIN_CABLE)
        return 0;
    if (sysex) {
        uint32_t k = cin == 4 ? 3 : cin - 4;
        for (uint32_t i = 0; i < k; i++)
            send_sysex(SRC_USB, ev[1 + i]);
    } else if (cin == 5) {
        send_short(SRC_USB, ev + 1, 1);     /* F6 */
    } else if (len[cin]) {
        send_short(SRC_USB, ev + 1, len[cin]);
    }
    return 1;
#elif GM_DIN_MODE == GM_DIN_MERGE
    /* Channel and realtime only: SysEx on the main port is the gemifi
     * protocol's. */
    if (cable == 0 && gm_cfg.din_usb && !sysex && cin >= 8 && len[cin])
        send_short(SRC_USB, ev + 1, len[cin]);
    return 0;
#else
    (void)cable;
    (void)sysex;
    (void)len;
    return 0;
#endif
}

#else /* !GM_DIN */

void gm_din_init(void)
{
}

void gm_din_poll(uint32_t now)
{
    (void)now;
}

void gm_din_local(const uint8_t *m, uint32_t n)
{
    (void)m;
    (void)n;
}

int gm_din_usb(const uint8_t ev[4])
{
    (void)ev;
    return 0;
}

#endif
