/*
 * usb_midi.c: USB-MIDI 1.0 event packing on top of usbd (copied from
 * platform/nuvoton-nuc1xx/src/usb_midi.c, only the MIDI_TX_PKTS default
 * differs; keep the two in sync).
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

/* A packet the host hasn't taken after this long: nobody reads the IN
 * endpoint (no app has the port open, or the bus is suspended). */
#ifndef MIDI_TX_TIMEOUT_MS
#define MIDI_TX_TIMEOUT_MS 50
#endif

/* Transmit packets queued behind the one in flight (that one is already in
 * USB packet RAM), a power of 2. 0 = one buffer that blocks when full and
 * no host-absent tracking (the NUC bootloader, which has no room for more).
 * 4 here; the NUC copy has 2 (4 KB of RAM). */
#ifndef MIDI_TX_PKTS
#define MIDI_TX_PKTS 4
#endif
#if MIDI_TX_PKTS & (MIDI_TX_PKTS - 1)
#error "MIDI_TX_PKTS must be 0 or a power of 2"
#endif

static uint8_t rx_pos;              /* next event in the current OUT packet */

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

#if MIDI_TX_PKTS == 0

static uint8_t tx_buf[64];
static uint8_t tx_n;

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

#else

/* A ring of packets: tx_cnt in use from tx_head on; the newest one is being
 * filled and holds tx_n bytes, the others are full. */
static uint8_t tx_pkt[MIDI_TX_PKTS][64];
static uint8_t tx_head, tx_cnt, tx_n;
/* The host stopped reading: everything is dropped, without waiting, while
 * a zero-length probe packet sits in the IN endpoint. The host taking it
 * (usbd_in_busy() going 0: an IN completion, or a bus reset) ends this. */
static uint8_t tx_absent;
static uint32_t tx_sent_ms;         /* when the packet in flight was armed */

static void tx_drop(void)
{
    tx_cnt = 0;
    tx_n = 0;
}

/* The host hasn't taken the packet in flight: drop it and the queue, and
 * leave a zero-length packet armed to see when it reads again (sending
 * stale events then could start notes whose Note Off was dropped). */
static void tx_lost(void)
{
    usbd_in_cancel();
    tx_drop();
    tx_absent = 1;
    if (usbd_configured())
        usbd_in_write(tx_pkt[0], 0);
}

/* 1 if the host reads. resume = 1: the next event starts a message, so it
 * may end the host-absent state (never mid-SysEx: the host would get the
 * tail of a message whose start was dropped). */
static int tx_host(int resume)
{
    if (tx_absent && resume && !usbd_in_busy())
        tx_absent = 0;
    return !tx_absent;
}

/* Hand the oldest queued packet to the IN endpoint if it is free. */
static int tx_send(void)
{
    if (!tx_cnt || usbd_in_busy() || !usbd_configured())
        return 0;
    usbd_in_write(tx_pkt[tx_head], tx_cnt == 1 ? tx_n : 64u);
    tx_sent_ms = sys_tick_ms();
    tx_head = (uint8_t)((tx_head + 1u) & (MIDI_TX_PKTS - 1u));
    if (!--tx_cnt)
        tx_n = 0;
    return 1;
}

int midi_tx_flush(uint32_t timeout_ms)
{
    uint32_t t0 = sys_tick_ms();
    while (tx_cnt) {
        if (!tx_host(1) || !usbd_configured()) {
            tx_drop();
            return -1;
        }
        if (!tx_send()) {
            if (sys_tick_ms() - t0 >= timeout_ms) {
                tx_lost();
                return -1;
            }
            usbd_poll();
        }
    }
    return 0;
}

void midi_tx_poll(void)
{
    if (!tx_host(1))
        return;
    if (!tx_send() && usbd_in_busy() &&
        sys_tick_ms() - tx_sent_ms >= MIDI_TX_TIMEOUT_MS)
        tx_lost();
}

void midi_tx_event(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint32_t c = cin & 0x0Fu;
    if (!tx_host(c < 4 || c > 7 || b0 == 0xF0))
        return;
    if (!tx_cnt || tx_n >= 64) {
        /* Ring full: wait for the host to take a packet, at most until the
         * one in flight is MIDI_TX_TIMEOUT_MS old. */
        while (tx_cnt == MIDI_TX_PKTS) {
            if (!usbd_configured()) {
                tx_drop();
                break;
            }
            if (tx_send())
                break;
            if (sys_tick_ms() - tx_sent_ms >= MIDI_TX_TIMEOUT_MS) {
                tx_lost();
                return;
            }
            usbd_poll();
        }
        tx_cnt++;
        tx_n = 0;
    }
    uint8_t *p = tx_pkt[(tx_head + tx_cnt - 1u) & (MIDI_TX_PKTS - 1u)] + tx_n;
    p[0] = cin;
    p[1] = b0;
    p[2] = b1;
    p[3] = b2;
    tx_n = (uint8_t)(tx_n + 4u);
}

#endif

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
