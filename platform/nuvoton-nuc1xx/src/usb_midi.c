/*
 * usb_midi.c: USB-MIDI 1.0 event packing/unpacking.
 *
 * USB-MIDI 1.0 section 4: each event is 4 bytes, byte 0 = cable << 4 | CIN.
 * SysEx: CIN 4 = starts/continues (3 bytes), 5 = ends with 1 byte,
 * 6 = ends with 2 bytes, 7 = ends with 3 bytes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "usb_midi.h"
#include "usbd.h"
#include "sys.h"

#ifndef MIDI_TX_TIMEOUT_MS
#define MIDI_TX_TIMEOUT_MS 50
#endif

static uint8_t rx_pos;              /* next event in the current OUT packet */
static uint8_t tx_buf[64];
static uint8_t tx_n;

int midi_rx_poll(gsx_rx_t *rx)
{
    int n = usbd_out_len();
    if (n < 0) {
        rx_pos = 0;
        return 0;
    }
    const volatile uint8_t *p = usbd_out_data();
    while (rx_pos + 4u <= (uint32_t)n) {
        uint32_t cin = p[rx_pos] & 0x0F;
        uint32_t k = 0;
        if (cin >= 4 && cin <= 7)
            k = (cin == 5) ? 1 : (cin == 6) ? 2 : 3;
        const volatile uint8_t *e = p + rx_pos + 1;
        rx_pos += 4;
        int done = 0;
        for (uint32_t i = 0; i < k; i++)
            done |= gsx_rx_byte(rx, e[i]);
        if (done)
            return 1;
    }
    rx_pos = 0;
    usbd_out_release();
    return 0;
}

void midi_rx_drain(void (*cb)(const uint8_t ev[4]))
{
    int n = usbd_out_len();
    if (n < 0)
        return;
    const volatile uint8_t *p = usbd_out_data();
    for (int i = 0; i + 4 <= n; i += 4) {
        uint8_t ev[4] = { p[i], p[i + 1], p[i + 2], p[i + 3] };
        cb(ev);
    }
    rx_pos = 0;
    usbd_out_release();
}

int midi_tx_flush(uint32_t timeout_ms)
{
    if (!tx_n)
        return 0;
    uint32_t t0 = sys_tick_ms();
    while (usbd_in_busy() && usbd_configured()) {
        usbd_poll();
        if (sys_tick_ms() - t0 >= timeout_ms) {
            /* Nobody reads the IN endpoint (no port open on the host):
             * withdraw the stale packet and drop ours. */
            usbd_in_cancel();
            tx_n = 0;
            return -1;
        }
    }
    if (!usbd_configured()) {
        tx_n = 0;
        return -1;
    }
    usbd_in_write(tx_buf, tx_n);
    tx_n = 0;
    return 0;
}

void midi_tx_poll(void)
{
    if (tx_n && usbd_configured() && !usbd_in_busy()) {
        usbd_in_write(tx_buf, tx_n);
        tx_n = 0;
    }
}

void midi_tx_event(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2)
{
    if (tx_n >= sizeof(tx_buf))
        midi_tx_flush(MIDI_TX_TIMEOUT_MS);
    tx_buf[tx_n++] = cin;
    tx_buf[tx_n++] = b0;
    tx_buf[tx_n++] = b1;
    tx_buf[tx_n++] = b2;
}

void midi_tx_sysex(const uint8_t *p, uint32_t n)
{
    midi_tx_sysex_cable(0, p, n);
}

void midi_tx_sysex_cable(uint8_t cable, const uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t k = n > 3 ? 3 : n;
        uint8_t cin = (n > 3) ? 4 : (uint8_t)(4 + k);   /* 4, or 5/6/7 */
        midi_tx_event((uint8_t)(cable << 4 | cin), p[0], k > 1 ? p[1] : 0, k > 2 ? p[2] : 0);
        p += k;
        n -= k;
    }
}
