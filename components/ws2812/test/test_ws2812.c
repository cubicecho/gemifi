/*
 * test_ws2812.c: host test of the ws2812 hub (vpin.c) with
 * core/test/ws2812-test.yaml: an RGB strip (GRB) and an RGBW one (GRBW,
 * gamma 2.2, white). The simulated HAL (hal_ledstrip_*) keeps each strip's
 * frames; the test checks the bytes and their order, gamma and white, the
 * RGBW white part, that a frame only goes out when something changed, at
 * most every WS_FRAME_MS (5 ms) and never while the last is busy, and the
 * full-colour looks, led_defaults and cycle step colours on strip LEDs.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_presets.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "gm_vpin.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

/* stm32f1 pin numbers: PA = 0.., PB = 16.., PC = 32.. */
#define B0   16                 /* PB0 */
#define B1   17                 /* PB1 */
#define BCYC 19                 /* PB3 */
#define STRIP 31                /* PB15: the RGB strip, 4 LEDs */
#define RGBW  7                 /* PA7: the RGBW strip, 2 LEDs */

static uint32_t now_ms;
static uint8_t level[64];

/* a strip: what open got, the last frame, how many, and when */
typedef struct {
    int open;
    uint32_t n_open;
    uint8_t frame[64];
    uint32_t n;
    int frames;
    uint32_t t_last, min_gap;
    int busy;                   /* the test holds it busy */
    int writes_busy;            /* writes while busy (must be 0) */
} strip_t;
static strip_t strips[2];

static strip_t *strip_of(uint16_t pin)
{
    return pin == STRIP ? &strips[0] : pin == RGBW ? &strips[1] : 0;
}

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { if (pin < 64) level[pin] = (uint8_t)(l != 0); }
int hal_pin_read(uint16_t pin) { return pin < 64 ? level[pin] : 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n) { (void)cable; (void)m; (void)n; }
void hal_midi_send(const uint8_t *m, uint32_t n) { (void)m; (void)n; }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

int hal_ledstrip_open(uint16_t pin, uint32_t n)
{
    strip_t *s = strip_of(pin);
    if (!s)
        return 0;
    s->open++;
    s->n_open = n;
    return 1;
}

int hal_ledstrip_busy(uint16_t pin)
{
    strip_t *s = strip_of(pin);
    return s && s->busy;
}

int hal_ledstrip_write(uint16_t pin, const uint8_t *b, uint32_t n)
{
    strip_t *s = strip_of(pin);
    if (!s || n > sizeof s->frame)
        return 0;
    if (s->busy) {
        s->writes_busy++;
        return 0;
    }
    if (s->frames && now_ms - s->t_last < s->min_gap)
        s->min_gap = now_ms - s->t_last;
    memcpy(s->frame, b, n);
    s->n = n;
    s->frames++;
    s->t_last = now_ms;
    return 1;
}

/* ---- helpers ---------------------------------------------------------- */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

static void press(uint16_t pin) { level[pin] = 0; run_ms(30); }     /* inverted, pull-up */
static void release(uint16_t pin) { level[pin] = 1; run_ms(30); }

/* LED led of a strip's last frame as 0xAABBCC(DD): its bytes in wire order */
static uint32_t wire(const strip_t *s, uint32_t led, uint32_t ch)
{
    uint32_t v = 0;
    for (uint32_t k = 0; k < ch; k++)
        v = v << 8 | s->frame[led * ch + k];
    return v;
}

/* The RGBW strip's byte for colour c at value v: its curve, then its white */
static uint32_t out_w(uint32_t c, uint32_t v)
{
    const gm_hub_t *h = &gm_device.hubs[1];
    uint32_t x = h->ws.curve ? h->ws.curve[v] : v;
    uint32_t w = h->ws.white[c] ? h->ws.white[c] : 255u;
    if (w == 255u || !x)
        return x;
    x = (x * w + 127u) / 255u;
    return x ? x : 1u;
}

static uint32_t grbw(uint32_t r, uint32_t g, uint32_t b, uint32_t w)
{
    return out_w(1, g) << 24 | out_w(0, r) << 16 | out_w(2, b) << 8 | out_w(3, w);
}

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    level[B0] = level[B1] = level[BCYC] = 1;    /* released */
    strips[0].min_gap = strips[1].min_gap = 0xFFFFFFFFu;
    printf("WS2812 strips (%u LEDs)\n", (unsigned)GM_N_LEDS);
    CHECK(gm_device.hubs[0].type == GM_HUB_WS2812 && gm_device.hubs[1].type == GM_HUB_WS2812, "two ws2812 hubs");
    CHECK(gm_device.hubs[0].ws.order == 0x21 && gm_device.hubs[1].ws.order == 0xE1, "orders GRB, GRBW");

    gm_init(0x01000000u, 0);
    CHECK(strips[0].open == 1 && strips[0].n_open == 12, "RGB strip opened for 4 x 3 bytes (%u)", strips[0].n_open);
    CHECK(strips[1].open == 1 && strips[1].n_open == 8, "RGBW strip opened for 2 x 4 bytes (%u)", strips[1].n_open);
    CHECK(vpin_caps() & VPIN_RGB, "a full-colour hub: VPIN_RGB");

    run_ms(400);                /* past the lamp test */
    CHECK(strips[0].frames > 0 && strips[0].n == 12, "RGB strip: frames of 12 bytes");
    CHECK(strips[0].min_gap >= 5 || strips[0].frames < 2, "frames at least 5 ms apart (%u)", strips[0].min_gap);

    /* the looks, in GRB: l0 idle blue 20%, l1 led_defaults green 10%, LED 2
     * unused, the cycle's LED its first step (#ff8000) */
    CHECK(wire(&strips[0], 0, 3) == 0x000033, "l0 idle blue 20%%: G 00 R 00 B 33 (%06X)", wire(&strips[0], 0, 3));
    CHECK(wire(&strips[0], 1, 3) == 0x1A0000, "l1 led_defaults green 10%%: G 1A (%06X)", wire(&strips[0], 1, 3));
    CHECK(wire(&strips[0], 2, 3) == 0, "LED 2: nothing on it, dark (%06X)", wire(&strips[0], 2, 3));
    CHECK(wire(&strips[0], 3, 3) == 0x80FF00, "cycle step #ff8000: G 80 R FF (%06X)", wire(&strips[0], 3, 3));
    /* RGBW: #402010 -> white part 10: r 30, g 10, b 0, w 10, through
     * gamma 2.2 and white [255, 200, 220, 128]; LED 1's white pin idle on */
    CHECK(wire(&strips[1], 0, 4) == grbw(0x30, 0x10, 0, 0x10), "RGBW #402010: white part on W (%08X, want %08X)",
          wire(&strips[1], 0, 4), grbw(0x30, 0x10, 0, 0x10));
    CHECK(wire(&strips[1], 1, 4) == grbw(0, 0, 0, 255) && (wire(&strips[1], 1, 4) & 0xFF) == 128,
          "RGBW LED 1: its white pin on, white: 128 (%08X)", wire(&strips[1], 1, 4));
    CHECK(out_w(0, 64) == gm_device.hubs[1].ws.curve[64] && gm_device.hubs[1].ws.curve[64] < 20,
          "gamma 2.2: 64 is about 13 (%u)", out_w(0, 64));
    CHECK(level[32] == 1, "an MCU LED with led_defaults green 10%%: on");

    /* only changed frames: nothing changes, nothing is sent */
    int f0 = strips[0].frames, f1 = strips[1].frames;
    run_ms(1000);
    CHECK(strips[0].frames == f0 && strips[1].frames == f1, "steady: no frames (%d, %d more)",
          strips[0].frames - f0, strips[1].frames - f1);

    /* a press: one frame with l0's active #ff8000, the other strip quiet */
    press(B0);
    CHECK(strips[0].frames == f0 + 1, "press: one frame (%d)", strips[0].frames - f0);
    CHECK(wire(&strips[0], 0, 3) == 0x80FF00, "l0 active #ff8000 (%06X)", wire(&strips[0], 0, 3));
    CHECK(strips[1].frames == f1, "the other strip: no frame");
    release(B0);
    CHECK(wire(&strips[0], 0, 3) == 0x000033, "l0 idle again (%06X)", wire(&strips[0], 0, 3));
    press(B1);
    CHECK(wire(&strips[1], 0, 4) == grbw(0, 0, 0, 255), "RGBW active white: the white LED only (%08X)",
          wire(&strips[1], 0, 4));
    release(B1);

    /* rate limit: a colour every 1 ms for 50 ms, at most a frame per 5 ms,
     * and the last colour shows */
    gm_pin_t led2 = { 1, 0, 6 };
    f0 = strips[0].frames;
    strips[0].min_gap = 0xFFFFFFFFu;
    for (uint32_t i = 1; i <= 50; i++) {
        vpin_rgb(led2, i);
        run_ms(1);
    }
    run_ms(10);
    int n = strips[0].frames - f0;
    CHECK(n >= 9 && n <= 11, "50 ms of changes: about 10 frames (%d)", n);
    CHECK(strips[0].min_gap >= 5, "at least 5 ms apart (%u)", strips[0].min_gap);
    CHECK(wire(&strips[0], 2, 3) == 50, "the last colour shows (%06X)", wire(&strips[0], 2, 3));

    /* changed and back before the frame: nothing to send */
    f0 = strips[0].frames;
    vpin_rgb(led2, 0x123456);
    vpin_rgb(led2, 50);
    run_ms(20);
    CHECK(strips[0].frames == f0, "changed and back: no frame (%d)", strips[0].frames - f0);

    /* busy: no write while the last frame goes out; then the new one */
    strips[0].busy = 1;
    vpin_rgb(led2, 0xFF0000);
    run_ms(30);
    CHECK(strips[0].frames == f0 && strips[0].writes_busy == 0, "busy: nothing written (%d, %d)",
          strips[0].frames - f0, strips[0].writes_busy);
    strips[0].busy = 0;
    run_ms(2);
    CHECK(strips[0].frames == f0 + 1 && wire(&strips[0], 2, 3) == 0x00FF00, "not busy: the frame (G 00 R FF, %06X)",
          wire(&strips[0], 2, 3));

    /* pin writes: on is 255, off 0 (pin = LED * 3 + colour) */
    gm_pin_t g2 = { 1, 0, 7 };
    vpin_write(g2, 1);
    run_ms(10);
    CHECK(wire(&strips[0], 2, 3) == 0xFFFF00, "LED 2 green pin on: G FF (%06X)", wire(&strips[0], 2, 3));
    CHECK(vpin_rgb((gm_pin_t){ 1, 0, 7 }, 0xFFFFFF) == 0, "vpin_rgb on a green pin: no (0)");
    CHECK(vpin_rgb((gm_pin_t){ 2, 0, 4 }, 0x0000FF) == 1, "RGBW LED 1's red pin (4): yes");

    /* the cycle: full step colours, a named one with pins */
    press(BCYC);
    release(BCYC);
    CHECK(wire(&strips[0], 3, 3) == 0x000033, "step 2, blue 20%% (%06X)", wire(&strips[0], 3, 3));
    press(BCYC);
    release(BCYC);
    CHECK(wire(&strips[0], 3, 3) == 0x00FF00, "step 3, red: the red pin (%06X)", wire(&strips[0], 3, 3));
    press(BCYC);
    release(BCYC);
    CHECK(wire(&strips[0], 3, 3) == 0x80FF00, "round to step 1, #ff8000 (%06X)", wire(&strips[0], 3, 3));

    printf("test_ws2812: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
