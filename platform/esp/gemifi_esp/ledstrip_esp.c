/*
 * ledstrip_esp.c: LED strips (gm_hal.h hal_ledstrip_*, the ws2812 hub) by
 * RMT, ESP-IDF's TX driver with a bytes encoder.
 *
 * Each strip has an RMT TX channel at a 10 MHz tick (0.1 us) and a copy of
 * its frame (the driver reads it while it sends). WS2812B / SK6812 timings:
 * a 0 is 0.4 us high then 0.8 us low, a 1 is 0.8 us high then 0.5 us low
 * (inside both parts' windows), MSB first, the line idling low. The latch
 * is the line low for LS_LATCH_US after the frame: hal_ledstrip_busy is 1
 * until then, so "not busy" means the LEDs show the frame.
 * hal_ledstrip_write starts the frame and returns; the driver feeds the
 * channel from its interrupt while the main loop goes on. A frame of 16 LEDs
 * is about 0.8 ms on the wire.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include "gm_esp.h"
#include "gm_hal.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#if SOC_RMT_SUPPORTED

#define LS_MAX 8                    /* strips (ESP32 has 8 RMT TX channels, S2 / S3 4) */
#define LS_TICK_HZ 10000000u        /* 0.1 us */
#define LS_LATCH_US 300

/* How many of the RMT's memory blocks each strip's channel takes: more
 * means fewer refills from the interrupt, so a late refill (radio, flash)
 * is less likely to cut a frame short. One strip can have two. */
#ifndef GM_LEDSTRIPS
#define GM_LEDSTRIPS 2
#endif
#define LS_BLOCKS (GM_LEDSTRIPS > 1 ? 1 : 2)

typedef struct {
    int16_t pin;                    /* -1 = free */
    uint32_t n;                     /* bytes a frame, at most */
    uint8_t *buf;                   /* the frame being sent */
    rmt_channel_handle_t ch;
    rmt_encoder_handle_t enc;
    volatile uint8_t sending;
    volatile int64_t t_done;        /* when the last frame ended (us) */
} strip_t;

static strip_t strips[LS_MAX] = {
    { .pin = -1 }, { .pin = -1 }, { .pin = -1 }, { .pin = -1 },
    { .pin = -1 }, { .pin = -1 }, { .pin = -1 }, { .pin = -1 },
};

static strip_t *strip_of(uint16_t pin)
{
    for (uint32_t i = 0; i < LS_MAX; i++)
        if (strips[i].pin == (int16_t)pin)
            return &strips[i];
    return NULL;
}

static bool done_cb(rmt_channel_handle_t ch, const rmt_tx_done_event_data_t *ev, void *arg)
{
    (void)ch;
    (void)ev;
    strip_t *s = (strip_t *)arg;
    s->t_done = esp_timer_get_time();
    s->sending = 0;
    return false;                   /* no task to wake */
}

int hal_ledstrip_open(uint16_t pin, uint32_t n)
{
    strip_t *s = strip_of(pin);
    if (s)
        return n <= s->n;
    for (uint32_t i = 0; i < LS_MAX && !s; i++)
        if (strips[i].pin < 0)
            s = &strips[i];         /* a free slot */
    if (!s || n == 0)
        return 0;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf)
        return 0;
    rmt_tx_channel_config_t c = {
        .gpio_num = (gpio_num_t)pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LS_TICK_HZ,
        .mem_block_symbols = LS_BLOCKS * SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 1,
    };
    rmt_bytes_encoder_config_t e = {
        .bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 8 },
        .bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 5 },
        .flags.msb_first = 1,
    };
    rmt_tx_event_callbacks_t cb = { .on_trans_done = done_cb };
    rmt_channel_handle_t ch = NULL;
    rmt_encoder_handle_t enc = NULL;
    if (rmt_new_tx_channel(&c, &ch) != ESP_OK || rmt_new_bytes_encoder(&e, &enc) != ESP_OK ||
        rmt_tx_register_event_callbacks(ch, &cb, s) != ESP_OK || rmt_enable(ch) != ESP_OK) {
        if (enc)
            rmt_del_encoder(enc);
        if (ch)
            rmt_del_channel(ch);
        free(buf);
        return 0;
    }
    s->pin = (int16_t)pin;
    s->n = n;
    s->buf = buf;
    s->ch = ch;
    s->enc = enc;
    s->sending = 0;
    s->t_done = 0;
    return 1;
}

int hal_ledstrip_busy(uint16_t pin)
{
    strip_t *s = strip_of(pin);
    return s && (s->sending || esp_timer_get_time() - s->t_done < LS_LATCH_US);
}

int hal_ledstrip_write(uint16_t pin, const uint8_t *b, uint32_t n)
{
    strip_t *s = strip_of(pin);
    if (!s || n > s->n || hal_ledstrip_busy(pin))
        return 0;
    memcpy(s->buf, b, n);
    rmt_transmit_config_t t = { .loop_count = 0, .flags.eot_level = 0 };
    s->sending = 1;
    if (rmt_transmit(s->ch, s->enc, s->buf, n, &t) != ESP_OK) {
        s->sending = 0;
        return 0;
    }
    return 1;
}

#endif
