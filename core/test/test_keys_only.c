/*
 * test_keys_only.c: host test of a device with a key matrix and nothing
 * else (core/test/keys-only-test.yaml): the core builds without buttons,
 * LEDs, analog inputs or encoders (feedback on), a key plays its note, and
 * the raw readings stream the matrix.
 *
 * Rows PA0, PA1 (active-high), columns PB0..PB3; key 0 is row 0, columns
 * 0 and 1 (its two contacts), note 60.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "app_proto.h"
#include "gm_feedback.h"
#include "gsx.h"

#if !GM_HAS_MATRIX || GM_N_BUTTONS || GM_N_LEDS || GM_N_ANALOGS || GM_N_ENCODERS || !GM_FEEDBACK || !GM_MONITOR
#error "test_keys_only.c needs the keys-only-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

#define PIN_ROW 0                   /* PA0, PA1 */
#define PIN_COL 16                  /* PB0..PB3 */
static uint32_t now_ms;
static uint8_t level[64];
static uint8_t closed[2];           /* per row: the contacts closed, bit = column */

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin)
{
    if (pin >= PIN_COL && pin < PIN_COL + 4u) {
        for (uint32_t r = 0; r < 2u; r++)
            if (level[PIN_ROW + r] && ((closed[r] >> (pin - PIN_COL)) & 1u))
                return 1;
        return 0;
    }
    return level[pin];
}
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return 0; }

static uint8_t out[4096];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (cable == 0 && out_n + (int)n < (int)sizeof out) {
        memcpy(out + out_n, m, n);
        out_n += (int)n;
    }
}
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

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

/* Channel-voice messages of status st with data byte 1 = d1 sent since the
 * last clear (SysEx skipped); *last the last one's second data byte. */
static int count(uint8_t st, int d1, int *last)
{
    int n = 0;
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == st && out[i + 1] == d1) {
            n++;
            if (last)
                *last = out[i + 2];
        }
    }
    return n;
}

/* The payload of the first RAW message sent since the last clear, or 0. */
static const uint8_t *first_raw(void)
{
    for (int i = 0; i + 1 + GSX_OFF_PAYLOAD + 4 < out_n; i++)
        if (out[i] == 0xF0 && out[i + 1] == 0x7D && out[i + 1 + GSX_OFF_CMD] == GSX_MSG_RAW)
            return out + i + 1 + GSX_OFF_PAYLOAD;
    return 0;
}

/* ---- the tests -------------------------------------------------------- */

int main(void)
{
    int v = -1;
    gm_init(0x00010000u, 0);
    run_ms(500);
    out_n = 0;

    closed[0] = 0x3u;               /* key 0: both contacts */
    run_ms(20);
    CHECK(count(0x90, 60, &v) == 1 && v > 0, "key 0 down: Note On 60");
    out_n = 0;
    closed[0] = 0;
    run_ms(20);
    CHECK(count(0x80, 60, 0) + count(0x90, 60, &v) == 1, "key 0 up: Note Off 60");

    /* the raw readings: only the key matrix streams */
    CHECK(gm_monitor(GSX_MON_BUTTONS | GSX_MON_MATRIX | GSX_MON_ANALOG) == GSX_MON_MATRIX,
          "monitor: the key matrix only");
    closed[1] = 0x4u;               /* row 1, column 2: one contact of key 3 */
    out_n = 0;
    run_ms(200);
    const uint8_t *p = first_raw();
    /* kind, first row, rows, columns, then the contacts 7 to a byte */
    CHECK(p && p[0] == EV_KEY && p[1] == 0 && p[2] == 2 && p[3] == 4 && p[4] == 0x40 && p[5] == 0,
          "monitor: RAW of the key matrix, row 1 column 2 closed");

    printf("test_keys_only: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
