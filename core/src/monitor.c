/*
 * monitor.c: the raw readings (sysex.md 4.8, core/README.md "Raw
 * readings"): MONITOR from a host streams what the buttons, the key
 * matrix, the analog inputs, the encoders and the pads last read, for a
 * tester. The main loop (core/src/controller.c gm_poll) calls monitor_poll.
 * Reads the controls' state through gm_controls.h.
 *
 * MONITOR turns the stream on for a few seconds; gm_poll sends one RAW
 * message per GM_MONITOR_INTERVAL_MS, the kinds in turn, each its next
 * chunk. The readings are what the scan last saw (it isn't stopped or
 * locked: a reading a scan tick old is fine for a tester).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_device.h"
#include "gm_hal.h"
#include "gsx.h"

#if GM_MONITOR
#define MON_CHUNK 32u               /* values per message */
#define MON_BITS 512u               /* key-matrix contacts per message */
#define MON_KINDS 5u                /* GSX_MON_* bits */
static uint8_t mon_kinds;           /* the kinds streaming, 0 = off */
static uint8_t mon_turn;            /* the kind (bit) sending next */
static uint16_t mon_first;          /* its next chunk: first index (key matrix: row) */
static uint32_t mon_until, mon_t;
static const uint8_t mon_raw_kind[MON_KINDS] = { EV_BUTTON, EV_KEY, EV_ANALOG, EV_ENCODER, EV_PAD };

/* How many values (key matrix: rows) a kind has. */
static uint32_t mon_count(uint32_t b)
{
    switch (b) {
    case 0:
        return GM_N_OWN_BUTTONS;
#if GM_HAS_MATRIX
    case 1:
        return gm_device.matrix ? (gm_device.matrix->n_rows > FAST_ROWS ? FAST_ROWS : gm_device.matrix->n_rows) : 0u;
#endif
    case 2:
        return GM_N_ANALOGS;
    case 3:
        return NE_HW;
#if GM_N_PADS > 0
    case 4:
        return GM_N_PADS;
#endif
    default:
        return 0;
    }
}

uint32_t gm_monitor(uint32_t kinds)
{
    uint32_t have = 0;
    for (uint32_t b = 0; b < MON_KINDS; b++)
        if (mon_count(b))
            have |= 1u << b;
    kinds &= have;
    if (kinds != mon_kinds) {
        mon_turn = 0;
        mon_first = 0;
    }
    mon_kinds = (uint8_t)kinds;
    mon_until = hal_millis() + GM_MONITOR_TIMEOUT_MS;
    return kinds;
}

#if GM_N_BUTTONS > 0 || GM_HAS_MATRIX
/* Bits into 7-bit bytes, 7 per byte (bit i of byte j = bit 7j + i). Only
 * buttons and the key matrix send bits (a device with neither: unused). */
static uint32_t mon_bits(uint8_t *p, uint32_t nbits, uint32_t (*bit)(uint32_t))
{
    uint32_t n = (nbits + 6u) / 7u;
    for (uint32_t j = 0; j < n; j++)
        p[j] = 0;
    for (uint32_t b = 0; b < nbits; b++)
        if (bit(b))
            p[b / 7u] |= (uint8_t)(1u << (b % 7u));
    return n;
}
static uint32_t mon_first_bit;      /* mon_bits callbacks: index of bit 0 (key matrix: its row) */
#endif

#if GM_N_BUTTONS > 0
static uint32_t mon_button(uint32_t b)
{
    return (btn_hist[0] >> (mon_first_bit + b)) & 1u;
}
#endif
#if GM_HAS_MATRIX
static uint32_t mon_cols;
static uint32_t mon_contact(uint32_t b)
{
    return (mprev[mon_first_bit + b / mon_cols] >> (b % mon_cols)) & 1u;
}
#endif

/* One kind's value i (analog, encoder, pad) as a u14. */
static uint32_t mon_value(uint32_t b, uint32_t i)
{
    switch (b) {
#if GM_N_ANALOGS > 0
    case 2:
        return a_raw[i] < 0 ? 0x3FFFu : (uint32_t)a_raw[i];
#endif
#if GM_N_ENCODERS > 0
    case 3:
        return enc_count[i] & 0x3FFFu;
#endif
#if GM_N_PADS > 0
    case 4:
        return pad_raw[i];
#endif
    default:
        return 0;
    }
}

void monitor_poll(uint32_t now)
{
    if (!mon_kinds)
        return;
    if ((int32_t)(now - mon_until) >= 0) {
        mon_kinds = 0;              /* the host stopped renewing it */
        return;
    }
    if (now - mon_t < GM_MONITOR_INTERVAL_MS)
        return;
    mon_t = now;
    uint32_t b = mon_turn;
    while (!((mon_kinds >> b) & 1u))
        b = (b + 1u) % MON_KINDS;
    uint32_t total = mon_count(b), first = mon_first, n, cols = 1;
#if GM_HAS_MATRIX
    if (b == 1u) {
        cols = gm_device.matrix->n_cols > 32u ? 32u : gm_device.matrix->n_cols;
        if (!cols)
            cols = 1;
    }
#endif
    uint32_t chunk = b == 1u ? MON_BITS / cols : MON_CHUNK;
    n = total - first < chunk ? total - first : chunk;
    uint8_t p[4 + (MON_BITS + 6u) / 7u];
    _Static_assert(4u + 2u * MON_CHUNK <= sizeof(p), "RAW values");
    uint32_t len = 0;
    p[len++] = mon_raw_kind[b];
    if (b == 1u) {
#if GM_HAS_MATRIX
        p[len++] = (uint8_t)first;
        p[len++] = (uint8_t)n;
        p[len++] = (uint8_t)cols;
        mon_first_bit = first;
        mon_cols = cols;
        len += mon_bits(p + len, n * cols, mon_contact);
#endif
    } else {
        p[len++] = (uint8_t)(first & 0x7Fu);
        p[len++] = (uint8_t)(first >> 7);
        p[len++] = (uint8_t)n;
        if (b == 0u) {
#if GM_N_BUTTONS > 0
            mon_first_bit = first;
            len += mon_bits(p + len, n, mon_button);
#endif
        } else {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v = mon_value(b, first + i);
                p[len++] = (uint8_t)(v & 0x7Fu);
                p[len++] = (uint8_t)(v >> 7);
            }
        }
    }
    app_send_unsolicited(GSX_MSG_RAW, p, len);
    if (first + n < total) {
        mon_first = (uint16_t)(first + n);    /* the rest of this kind next time */
        return;
    }
    mon_first = 0;
    mon_turn = (uint8_t)((b + 1u) % MON_KINDS);
}
#endif

