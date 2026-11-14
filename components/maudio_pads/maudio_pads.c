/*
 * maudio_pads.c: the maudio_pads LED strip platform, the M-Audio Code pad
 * LED board: RGB LEDs behind a serial link with the board's own protocol
 * (components/maudio_pads/README.md). A hub type on the led_strip base
 * (components/led_strip): its pins are bytes in the hub RAM, and the
 * platform sends the messages (gm_hal.h hal_serial_*).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"
#include "led_strip.h"
#include "gm_vpin.h"

/* The platform's serial port; these weak ones say there is none. */
__attribute__((weak)) int hal_serial_open(uint16_t tx_pin, uint32_t baud, uint32_t flags)
{
    (void)tx_pin;
    (void)baud;
    (void)flags;
    return 0;
}

__attribute__((weak)) int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n)
{
    (void)tx_pin;
    (void)b;
    (void)n;
    return 0;
}

__attribute__((weak)) int hal_serial_read(uint16_t tx_pin)
{
    (void)tx_pin;
    return -1;
}

/* The M-Audio Code pad LED board (found in the Code 61 v1.2.2 app, 0x080115FC
 * and 0x08013C94): one LED per message, 02, 0x80 + LED, R, G, B, 03, with
 * 00 off and FF full; a lone A5 keepalive about every 60 ms (without it the
 * board turns its LEDs off). Messages sent back to back are lost after the
 * first, so every write, the A5 included, goes out RGB_GAP_MS after the
 * last one. */
#define RGB_GAP_MS 5u
#define RGB_KEEPALIVE_MS 50u
/* At power-on the board isn't listening yet: the first colours are lost
 * (the pads stayed dark until hit). So every LED is sent once more,
 * RGB_REPAINT_MS after start; after that an LED goes out only when it
 * changes. Resending one LED every 50 ms forever (builds 74-93) left the
 * board deaf after a cold start: with no LED message during its start-up
 * it comes up fine (tested on the Code 61, 2026-11-09). */
#ifndef RGB_REPAINT_MS
#define RGB_REPAINT_MS 2000u
#endif
/* With rx_pin: the board answers each LED message with 0x40 + LED (the
 * Code 61's does, about 220 ms after power-on at the earliest). An LED is
 * only counted as shown once answered: without an answer in RGB_ACK_MS it
 * is sent again, after a pause that grows with each miss (RGB_RETRY_MS
 * times the misses in a row, at most RGB_RETRY_MAX_MS), so a board that
 * is still starting gets little traffic. The first answer means the board
 * is up: every LED is sent again then, instead of at RGB_REPAINT_MS. With
 * no answer by RGB_ACK_GIVE_UP_MS the hub works as without rx_pin. */
#define RGB_ACK_MS 20u
#define RGB_RETRY_MS 50u
#define RGB_RETRY_MAX_MS 500u
#define RGB_ACK_GIVE_UP_MS 3000u
#define RGB_ACK 0x40u

static void mpads_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    /* All off; every LED is sent once (gm_hub_ram_sent starts different). */
    s->ram = gm_hub_ram_alloc(3u * h->mpads.count);
    for (uint32_t k = 0; k < 3u * h->mpads.count && s->ram + k < GM_HUB_RAM; k++) {
        gm_hub_ram[s->ram + k] = 0;
        gm_hub_ram_sent[s->ram + k] = 0xFFu;
    }
    if (h->mpads.tx.hub == 0)
        (void)hal_serial_open(h->mpads.tx.num, 115200u, (h->mpads.dma ? HAL_UART_DMA : 0u) |
                              (vpin_is_none(h->mpads.rx) ? 0u : HAL_UART_RX));
}

/* Pin num = LED * 3 + colour: on is 255, off 0. */
static void mpads_write(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, int level)
{
    if (num < 3u * h->mpads.count && s->ram + num < GM_HUB_RAM)
        gm_hub_ram[s->ram + num] = level ? 0xFFu : 0u;
}

static int mpads_rgb(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, uint32_t rgb)
{
    if (num % 3u || num + 3u > 3u * h->mpads.count || s->ram + num + 3u > GM_HUB_RAM)
        return 0;
    uint8_t *b = &gm_hub_ram[s->ram + num];
    b[0] = (uint8_t)(rgb >> 16);
    b[1] = (uint8_t)(rgb >> 8);
    b[2] = (uint8_t)rgb;
    return 1;
}

/* LED led's three bytes differ between a and b. */
static int rgb_differs(const uint8_t *a, const uint8_t *b, uint32_t led)
{
    a += led * 3u;
    b += led * 3u;
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
}

#define rgb_out(h, c, v) gm_led_strip_colour_out((h)->mpads.curve, (h)->mpads.white, (c), (v))

/* LED led no longer counts as sent: it goes out again. */
static void rgb_unsend(const uint8_t *ram, uint8_t *sent, uint32_t led)
{
    for (uint32_t c = 0; c < 3u; c++)
        sent[led * 3u + c] = (uint8_t)~ram[led * 3u + c];
}

/* Send one changed LED (at most every RGB_GAP_MS) and the keepalive. */
static void mpads_poll(const gm_hub_t *h, gm_hub_state_t *s, uint32_t now_ms)
{
    if (h->mpads.tx.hub != 0 || s->ram + 3u * h->mpads.count > GM_HUB_RAM)
        return;
    uint16_t tx = h->mpads.tx.num;
    const uint8_t *ram = &gm_hub_ram[s->ram];
    uint8_t *sent = &gm_hub_ram_sent[s->ram];
    int use_acks = !vpin_is_none(h->mpads.rx) && s->u.mpads.acks != 2u;
    int repaint = 0;
    if (use_acks) {
        for (int b; (b = hal_serial_read(tx)) >= 0;) {
            uint32_t led = (uint32_t)b - RGB_ACK;
            if (led >= h->mpads.count)
                continue;               /* not an answer */
            if (!s->u.mpads.acks) {
                s->u.mpads.acks = 1;
                repaint = !s->u.mpads.repainted;   /* the board is up: everything again */
            }
            if (s->u.mpads.wait_led == led + 1u) {
                s->u.mpads.wait_led = 0;
                s->u.mpads.misses = 0;
            }
        }
        if (s->u.mpads.wait_led && now_ms - s->u.mpads.t_led >= RGB_ACK_MS) {
            rgb_unsend(ram, sent, s->u.mpads.wait_led - 1u);     /* not answered: again */
            s->u.mpads.wait_led = 0;
            if (s->u.mpads.misses < 255u)
                s->u.mpads.misses++;
        }
        if (!s->u.mpads.acks && now_ms >= RGB_ACK_GIVE_UP_MS) {
            s->u.mpads.acks = 2;          /* never answered: as without rx_pin */
            s->u.mpads.wait_led = 0;
            s->u.mpads.misses = 0;
            use_acks = 0;
        }
    }
    if (!s->u.mpads.repainted && (repaint || (!use_acks && now_ms >= RGB_REPAINT_MS))) {
        for (uint32_t k = 0; k < 3u * h->mpads.count; k++)
            sent[k] = (uint8_t)~ram[k];     /* every LED differs: all sent again */
        s->u.mpads.repainted = 1;
    }
    if (now_ms - s->u.mpads.t_any < RGB_GAP_MS)
        return;
    if (now_ms - s->u.mpads.t_ka >= RGB_KEEPALIVE_MS) {
        static const uint8_t ka = 0xA5u;
        if (hal_serial_write(tx, &ka, 1))
            s->u.mpads.t_ka = s->u.mpads.t_any = now_ms;
        return;
    }
    if (use_acks && s->u.mpads.wait_led)
        return;                         /* the last LED isn't answered yet */
    if (use_acks && s->u.mpads.misses) {
        uint32_t pause = RGB_RETRY_MS * s->u.mpads.misses;
        if (now_ms - s->u.mpads.t_led < (pause < RGB_RETRY_MAX_MS ? pause : RGB_RETRY_MAX_MS))
            return;
    }
    /* The next changed LED after the last one sent, so one busy LED can't
     * starve the others. */
    uint32_t todo = h->mpads.count;       /* an LED to send, count = none */
    for (uint32_t k = 0; k < h->mpads.count; k++) {
        uint32_t led = ((uint32_t)(uint8_t)s->channel + 1u + k) % h->mpads.count;
        if (rgb_differs(ram, sent, led)) {
            todo = led;
            break;
        }
    }
    if (todo == h->mpads.count)
        return;
    const uint8_t *v = &ram[todo * 3u];
    uint8_t msg[6] = { 0x02u, (uint8_t)(0x80u + todo), rgb_out(h, 0, v[0]),
                       rgb_out(h, 1, v[1]), rgb_out(h, 2, v[2]), 0x03u };
    if (hal_serial_write(tx, msg, sizeof msg)) {
        for (uint32_t c = 0; c < 3u; c++)
            sent[todo * 3u + c] = v[c];
        s->channel = (int8_t)todo;
        s->u.mpads.t_any = s->u.mpads.t_led = now_ms;
        if (use_acks)
            s->u.mpads.wait_led = (uint8_t)(todo + 1u);
    }
}

const gm_hub_ops_t gm_maudio_pads_ops = {
    .flags = GM_HUBF_NO_READ, .init = mpads_init, .write = mpads_write, .rgb = mpads_rgb,
    .poll = mpads_poll,
};
