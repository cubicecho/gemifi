/*
 * ws2812.c: the ws2812 hub type, addressable LED strips (WS2812B, SK6812 RGB / RGBW) on one MCU pin
 * (components/ws2812/README.md). A hub type on the hub base (components/hub/gm_hub.h): its pins are
 * bytes in the hub RAM, and the platform sends the frames (gm_hal.h hal_ledstrip_*).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

/* The platform's LED strip driver; these weak ones say there is none (the builder refuses a ws2812
 * where the platform has none). */
__attribute__((weak)) int hal_ledstrip_open(uint16_t pin, uint32_t n)
{
    (void)pin;
    (void)n;
    return 0;
}

__attribute__((weak)) int hal_ledstrip_write(uint16_t pin, const uint8_t *b, uint32_t n)
{
    (void)pin;
    (void)b;
    (void)n;
    return 0;
}

__attribute__((weak)) int hal_ledstrip_busy(uint16_t pin)
{
    (void)pin;
    return 0;
}

/* An addressable LED chain (WS2812B, SK6812 RGB / RGBW) on one MCU pin.
 * The pins set a byte per colour in the hub RAM (on 255, off 0; vpin_rgb a
 * whole colour); the main loop (poll) builds a frame of wire bytes in
 * gm_hub_ram_sent and hands it to the platform (hal_ledstrip_write: RMT on ESP32,
 * SPI + DMA on stm32f1), which sends it in the background. A frame goes out
 * only when a colour changed and the frame differs from the last one, at
 * most every WS_FRAME_MS, and never while the last one (or its latch) is
 * still going. Never from the scan interrupt: a frame is 30 us an LED. */
#ifndef WS_FRAME_MS
#define WS_FRAME_MS 5u              /* at most 200 frames a second */
#endif

static void ws2812_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    uint32_t n = (uint32_t)h->ws.channels * h->ws.count;
    s->ram = gm_hub_ram_alloc(n);
    for (uint32_t k = 0; k < n && s->ram + k < GM_HUB_RAM; k++)
        gm_hub_ram[s->ram + k] = gm_hub_ram_sent[s->ram + k] = 0;
    s->u.ws.dirty = 1;              /* all off once: the strip may still show the last run's */
    s->u.ws.open = (uint8_t)(h->ws.pin.hub == 0 && s->ram + n <= GM_HUB_RAM &&
                             hal_ledstrip_open(h->ws.pin.num, n));
}

/* Pin num = LED * channels + colour: on is 255, off 0. */
static void ws2812_write(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, int level)
{
    if (num >= (uint32_t)h->ws.channels * h->ws.count || s->ram + num >= GM_HUB_RAM)
        return;
    uint8_t v = level ? 0xFFu : 0u;
    if (gm_hub_ram[s->ram + num] != v) {
        gm_hub_ram[s->ram + num] = v;
        s->u.ws.dirty = 1;
    }
}

/* A whole colour on the LED whose red pin is num (an RGBW LED's white byte
 * stays as its own pin has it). */
static int ws2812_rgb(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, uint32_t rgb)
{
    uint32_t ch = h->ws.channels;
    if (num % ch || num + ch > ch * h->ws.count || s->ram + num + ch > GM_HUB_RAM)
        return 0;
    uint8_t *b = &gm_hub_ram[s->ram + num];
    uint8_t c[3] = { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb };
    if (b[0] != c[0] || b[1] != c[1] || b[2] != c[2]) {
        b[0] = c[0];
        b[1] = c[1];
        b[2] = c[2];
        s->u.ws.dirty = 1;
    }
    return 1;
}

/* The next frame, if something changed: built in gm_hub_ram_sent (wire order,
 * curve and white applied). On an RGBW strip the white part of a colour
 * (the least of red, green and blue) goes on the white LED, with what the
 * white pin has. */
static void ws2812_poll(const gm_hub_t *h, gm_hub_state_t *s, uint32_t now_ms)
{
    uint32_t ch = h->ws.channels, n = ch * h->ws.count;
    if (!s->u.ws.dirty || !s->u.ws.open)
        return;
    if (s->u.ws.tried && now_ms - s->u.ws.t_frame < WS_FRAME_MS)
        return;
    if (hal_ledstrip_busy(h->ws.pin.num))
        return;
    const uint8_t *ram = &gm_hub_ram[s->ram];
    uint8_t *sent = &gm_hub_ram_sent[s->ram];
    int changed = s->never;         /* the strip doesn't have gm_hub_ram_sent yet */
    for (uint32_t led = 0; led < h->ws.count; led++) {
        const uint8_t *l = &ram[led * ch];
        uint32_t v[4] = { l[0], l[1], l[2], ch == 4u ? l[3] : 0u };
        if (ch == 4u) {
            uint32_t m = v[0] < v[1] ? v[0] : v[1];
            if (v[2] < m)
                m = v[2];
            v[0] -= m;
            v[1] -= m;
            v[2] -= m;
            v[3] = v[3] + m > 255u ? 255u : v[3] + m;
        }
        for (uint32_t k = 0; k < ch; k++) {
            uint32_t c = (h->ws.order >> (2u * k)) & 3u;
            uint8_t b = gm_hub_colour_out(h->ws.curve, h->ws.white, c, v[c]);
            if (sent[led * ch + k] != b) {
                sent[led * ch + k] = b;
                changed = 1;
            }
        }
    }
    s->u.ws.dirty = 0;
    if (!changed)
        return;                     /* changed and back: the strip has it */
    s->u.ws.tried = 1;
    s->u.ws.t_frame = now_ms;
    if (hal_ledstrip_write(h->ws.pin.num, sent, n)) {
        s->never = 0;
    } else {
        s->never = 1;               /* not sent: again in WS_FRAME_MS */
        s->u.ws.dirty = 1;
    }
}

const gm_hub_ops_t gm_ws2812_ops = {
    .flags = GM_HUBF_NO_READ, .init = ws2812_init, .write = ws2812_write, .rgb = ws2812_rgb,
    .poll = ws2812_poll,
};
