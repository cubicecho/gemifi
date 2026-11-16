/*
 * test_gestures.c: host test of the button gestures (components/buttons/
 * README.md "Gestures") with core/test/gestures-test.yaml, on fake time: a
 * button without one fires at the press; tap vs hold and the hold time.
 * Nothing a gesture sends is left hanging, a preset switch included.
 *
 * Buttons (PB0.., inverted): 0 plain CC 10, 1 tap_hold CC 20 / held CC 21,
 * 2 note_hold note 60 / held preset_next. button_settings: hold_time 300 ms.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"

#if GM_N_BUTTONS != 3 || GM_BTN_HOLD != 0x6
#error "test_gestures.c needs the gestures-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static uint8_t level[64];

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin) { return level[pin]; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return 0; }
int hal_i2c_write(uint8_t bus, uint8_t a, const uint8_t *p, uint32_t n) { (void)bus; (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t bus, uint8_t a, uint8_t *p, uint32_t n) { (void)bus; (void)a; (void)p; (void)n; return -1; }

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

enum { PLAIN, TAP_HOLD, NOTE_HOLD };
#define PIN_BTN(b) (16 + (b))       /* PB0.. */
/* A release counts once the contact has read open for BTN_STABLE (16)
 * samples: let go, then this long until it is seen. */
#define RELEASE_MS 20u
#define HOLD_MS 300u                /* button_settings.hold_time */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

/* Button b down (it counts at the next sample) or up (RELEASE_MS later). */
static void press(int b)
{
    level[PIN_BTN(b)] = 0;          /* inverted */
    run_ms(1);
}

static void release(int b)
{
    level[PIN_BTN(b)] = 1;
    run_ms(RELEASE_MS);
}

/* Messages with status st and data byte 1 = d1 (-1: any) since the last
 * clear, SysEx skipped; *last: the last one's second data byte. */
static int count(uint8_t st, int d1, int *last)
{
    int n = 0;
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == st && (d1 < 0 || out[i + 1] == d1)) {
            n++;
            if (last)
                *last = out[i + 2];
        }
    }
    return n;
}

/* The CC d1's values since the last clear, in order, as a string of
 * '1' (127) and '0' (0). */
static const char *cc_values(int d1)
{
    static char s[32];
    int j = 0;
    for (int i = 0; i + 2 < out_n && j < 31; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == 0xB0 && out[i + 1] == d1) {
            s[j++] = out[i + 2] ? '1' : '0';
            i += 2;
        }
    }
    s[j] = 0;
    return s;
}

/* ---- the tests -------------------------------------------------------- */

static void test_no_gesture(void)
{
    out_n = 0;
    press(PLAIN);
    CHECK(!strcmp(cc_values(10), "1"), "plain: CC 10 on at the press, no wait (got %s)", cc_values(10));
    release(PLAIN);
    CHECK(!strcmp(cc_values(10), "10"), "plain: CC 10 off at the release");
}

static void test_tap(void)
{
    out_n = 0;
    press(TAP_HOLD);
    run_ms(100);
    CHECK(out_n == 0, "tap_hold down: nothing yet (a tap or a hold?)");
    release(TAP_HOLD);
    CHECK(!strcmp(cc_values(20), "10"), "tap: its own CC 20, on and off at the release (got %s)",
          cc_values(20));
    CHECK(count(0xB0, 21, 0) == 0, "tap: no hold CC 21");

    out_n = 0;
    press(NOTE_HOLD);
    release(NOTE_HOLD);
    int v = -1;
    CHECK(count(0x90, 60, &v) == 1 && v > 0 && count(0x80, 60, 0) == 1, "note_hold tapped: note 60 on, then off");
    CHECK(gm_cfg.preset_active == 1, "note_hold tapped: no preset switch");
}

static void test_hold(void)
{
    out_n = 0;
    press(TAP_HOLD);
    run_ms(HOLD_MS - 5u);
    CHECK(out_n == 0, "tap_hold held %u ms: nothing yet", HOLD_MS - 5u);
    run_ms(10);
    CHECK(!strcmp(cc_values(21), "1"), "tap_hold held past hold_time: its hold CC 21 on (got %s)", cc_values(21));
    run_ms(500);
    CHECK(!strcmp(cc_values(21), "1"), "and only once");
    release(TAP_HOLD);
    CHECK(!strcmp(cc_values(21), "10"), "released: CC 21 off");
    CHECK(count(0xB0, 20, 0) == 0, "a hold sends no tap (CC 20)");

    /* button.hold_time is live: 600 ms now */
    gm_cfg.btn_hold_time = 600;
    out_n = 0;
    press(TAP_HOLD);
    run_ms(500);
    release(TAP_HOLD);
    CHECK(!strcmp(cc_values(20), "10") && count(0xB0, 21, 0) == 0, "hold_time 600: 500 ms is a tap");
    out_n = 0;
    press(TAP_HOLD);
    run_ms(610);
    CHECK(!strcmp(cc_values(21), "1"), "hold_time 600: 610 ms is a hold");
    release(TAP_HOLD);
    gm_cfg.btn_hold_time = HOLD_MS;
}

/* A preset switch from a hold lets go of the other hold's CC; nothing
 * hangs and nothing more comes at the releases. */
static void test_nothing_hangs(void)
{
    out_n = 0;
    press(TAP_HOLD);
    run_ms(HOLD_MS + 10u);
    CHECK(!strcmp(cc_values(21), "1"), "tap_hold held: CC 21 on");
    press(NOTE_HOLD);
    run_ms(HOLD_MS + 10u);
    CHECK(gm_cfg.preset_active == 2, "note_hold held: the next preset (got %d)", (int)gm_cfg.preset_active);
    CHECK(!strcmp(cc_values(21), "10"), "the preset switch let go of CC 21 (got %s)", cc_values(21));
    out_n = 0;
    release(NOTE_HOLD);
    release(TAP_HOLD);
    CHECK(count(0xB0, -1, 0) == 0 && count(0x90, -1, 0) == 0 && count(0x80, -1, 0) == 0,
          "released after the switch: silent");

    /* a press still being decided when the preset switches is dropped */
    out_n = 0;
    press(NOTE_HOLD);
    run_ms(HOLD_MS - 50u);
    press(TAP_HOLD);
    run_ms(60);
    CHECK(gm_cfg.preset_active == 3, "the next preset again");
    release(TAP_HOLD);
    release(NOTE_HOLD);
    CHECK(count(0xB0, 20, 0) == 0 && count(0xB0, 21, 0) == 0, "tap_hold, pressed before the switch: silent");
}

int main(void)
{
    for (int i = 0; i < 64; i++)
        level[i] = 1;               /* buttons up (inverted) */
    gm_init(0x00010000u, 0);
    run_ms(2000);

    test_no_gesture();
    test_tap();
    test_hold();
    test_nothing_hangs();

    printf("test_gestures: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
