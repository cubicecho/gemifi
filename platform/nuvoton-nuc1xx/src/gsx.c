/*
 * gsx.c: gemifi SysEx frame codec. See gsx.h and protocol/sysex.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gsx.h"

static const uint8_t gsx_id[GSX_ID_LEN] = { GSX_ID_BYTES };

int gsx_rx_byte(gsx_rx_t *rx, uint8_t b)
{
    if (b >= 0xF8)                  /* real-time: may appear anywhere */
        return 0;
    if (b == 0xF0) {
        rx->n = 0;
        rx->state = 1;
        return 0;
    }
    if (b == 0xF7)                  /* complete (state 1) or too long (2) */
        return rx->state != 0;
    if (b & 0x80) {                 /* any other status byte aborts */
        gsx_rx_reset(rx);
        return 0;
    }
    if (rx->state == 1) {
        if (rx->n < rx->cap)
            rx->buf[rx->n++] = b;
        else
            rx->state = 2;          /* too long: dropped at F7 */
    }
    return 0;
}

int gsx_parse(const gsx_rx_t *rx, gsx_frame_t *f)
{
    const uint8_t *body = rx->buf;
    uint32_t n = rx->n;
    if (n < GSX_OVERHEAD)
        return GSX_NOT_OURS;
    for (uint32_t i = 0; i < GSX_ID_LEN; i++)
        if (body[i] != gsx_id[i])
            return GSX_NOT_OURS;
    f->cmd = body[GSX_OFF_CMD];
    if (f->cmd & GSX_REPLY)         /* a reply or unsolicited message: */
        return GSX_NOT_OURS;        /* never answer it (no echo loops)  */
    f->seq = body[GSX_OFF_SEQ];
    if (rx->state == 2)             /* longer than our buffer */
        return GSX_BAD_LEN;
    f->len = (uint16_t)gsx_get_u(body + GSX_OFF_LEN, 2);
    f->payload = body + GSX_OFF_PAYLOAD;
    if ((uint32_t)f->len + GSX_OVERHEAD != n)
        return GSX_BAD_LEN;
    uint32_t sum = 0;
    for (uint32_t i = GSX_OFF_VER; i < n - 1u; i++)
        sum += body[i];
    if ((sum & 0x7F) != body[n - 1])
        return GSX_BAD_CHECKSUM;
    if (body[GSX_OFF_VER] != GSX_PROTO_VERSION)
        return GSX_BAD_CMD;
    return GSX_OK;
}

uint32_t gsx_get_u(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++)
        v |= (uint32_t)(p[i] & 0x7F) << (7u * i);
    return v;
}

int gsx_unpack(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t o = 0;
    while (n) {
        uint32_t msb = *src++;
        n--;
        uint32_t k = n < 7 ? n : 7;
        if (!k)
            return -1;
        for (uint32_t i = 0; i < k; i++)
            dst[o++] = (uint8_t)(src[i] | (((msb >> i) & 1u) << 7));
        src += k;
        n -= k;
    }
    return (int)o;
}

void gsx_tx_begin(gsx_tx_t *t, uint8_t *buf, uint8_t cmd, uint8_t seq)
{
    t->buf = buf;
    t->n = 0;
    buf[t->n++] = 0xF0;
    for (uint32_t i = 0; i < GSX_ID_LEN; i++)
        buf[t->n++] = gsx_id[i];
    buf[t->n++] = GSX_PROTO_VERSION;
    buf[t->n++] = cmd;
    buf[t->n++] = seq;
    t->n += 2;                      /* length, filled by gsx_tx_end */
}

void gsx_put_u(gsx_tx_t *t, uint32_t v, uint32_t n)
{
    while (n--) {
        t->buf[t->n++] = v & 0x7F;
        v >>= 7;
    }
}

void gsx_put_packed(gsx_tx_t *t, const uint8_t *src, uint32_t n)
{
    while (n) {
        uint32_t k = n < 7 ? n : 7;
        uint8_t *msb = &t->buf[t->n++];
        *msb = 0;
        for (uint32_t i = 0; i < k; i++) {
            *msb |= (uint8_t)((src[i] >> 7) << i);
            t->buf[t->n++] = src[i] & 0x7F;
        }
        src += k;
        n -= k;
    }
}

uint16_t gsx_tx_end(gsx_tx_t *t)
{
    /* body starts at buf[1]; payload length excludes the header */
    uint32_t plen = (uint32_t)t->n - 1u - GSX_OFF_PAYLOAD;
    t->buf[1 + GSX_OFF_LEN] = plen & 0x7F;
    t->buf[1 + GSX_OFF_LEN + 1] = (plen >> 7) & 0x7F;
    uint32_t sum = 0;
    for (uint32_t i = 1 + GSX_OFF_VER; i < t->n; i++)
        sum += t->buf[i];
    t->buf[t->n++] = sum & 0x7F;
    t->buf[t->n++] = 0xF7;
    return t->n;
}
