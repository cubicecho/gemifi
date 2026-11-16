/*
 * test_gestures.c: host test of the button gestures (components/buttons/
 * README.md "Gestures") with core/test/gestures-test.yaml, on fake time: a
 * button without one fires at the press; tap vs hold and the hold time;
 * double taps, and a single tap that waits for the double-tap window;
 * combos, and combo buttons pressed too far apart; a one-shot layer. Nothing a gesture sends is left hanging, a preset switch included.
 *
 * Buttons (PB0.., inverted): 0 plain CC 10, 1 tap_hold CC 20 / held CC 21,
 * 2 note_hold note 60 / held preset_next, 3 dbl note 62 / twice note 63,
 * 4 both CC 30 / twice CC 31 / held CC 32, 5..7 c1 CC 40, c2 CC 41, c3
 * CC 43, 8 fn: the one-shot fn_layer, plain's twin CC 11. Combos: c1 + c2
 * CC 42, c1 + c2 + c3 note 70. button_settings:
 * tap_time 200 ms, hold_time 300 ms, combo_time 50 ms.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"

#if GM_N_BUTTONS != 10 || GM_BTN_HOLD != 0x16 || GM_BTN_DOUBLE != 0x18 || GM_N_COMBOS != 2 || !GM_MODE_ONE_SHOT
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

enum { PLAIN, TAP_HOLD, NOTE_HOLD, DBL, BOTH, C1, C2, C3, FN };
#define PIN_BTN(b) (16 + (b))       /* PB0.. */
#define PIN_FN_LED 48               /* PD0 */
/* A release counts once the contact has read open for BTN_STABLE (16)
 * samples: let go, then this long until it is seen. */
#define RELEASE_MS 20u
#define HOLD_MS 300u                /* button_settings.hold_time */
#define TAP_MS 200u                 /* button_settings.tap_time */
#define COMBO_MS 50u                /* button_settings.combo_time */

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

/* A note's on (1) and off (0) since the last clear, in order. */
static const char *note_values(int note)
{
    static char s[32];
    int j = 0;
    for (int i = 0; i + 2 < out_n && j < 31; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        int is_on = out[i] == 0x90 && out[i + 1] == note && out[i + 2] > 0;
        int is_off = (out[i] == 0x80 || out[i] == 0x90) && out[i + 1] == note && !is_on;
        if (is_on || is_off) {
            s[j++] = is_on ? '1' : '0';
            i += 2;
        }
    }
    s[j] = 0;
    return s;
}

static void test_double_tap(void)
{
    /* a single tap waits for tap_time after the release, then goes out */
    out_n = 0;
    press(DBL);
    release(DBL);
    run_ms(TAP_MS - 10u);
    CHECK(out_n == 0, "dbl tapped: nothing within tap_time (it may be a double tap)");
    run_ms(20);
    CHECK(!strcmp(note_values(62), "10") && !*note_values(63), "dbl: a single tap after tap_time, note 62 (got %s)",
          note_values(62));

    /* pressed again within tap_time: the double tap's note, held while the button is */
    out_n = 0;
    press(DBL);
    release(DBL);
    run_ms(50);
    press(DBL);
    CHECK(!strcmp(note_values(63), "1") && !*note_values(62), "dbl twice: note 63 on at the second press");
    run_ms(400);
    release(DBL);
    run_ms(TAP_MS + 50u);
    CHECK(!strcmp(note_values(63), "10") && !*note_values(62), "released: note 63 off, and no tap after");

    /* another button's press doesn't wait: the tap goes out first */
    out_n = 0;
    press(DBL);
    release(DBL);
    press(PLAIN);
    CHECK(!strcmp(note_values(62), "10") && !strcmp(cc_values(10), "1"), "dbl tapped, then plain: the tap at once");
    CHECK(out[0] == 0x90 && out[1] == 62, "and before plain's CC 10");
    release(PLAIN);

    /* held for tap_time: not a double tap's first, the button's own note, held */
    out_n = 0;
    press(DBL);
    run_ms(TAP_MS + 10u);
    CHECK(!strcmp(note_values(62), "1"), "dbl held: its own note 62 on after tap_time");
    run_ms(500);
    release(DBL);
    run_ms(TAP_MS + 50u);
    CHECK(!strcmp(note_values(62), "10") && !*note_values(63), "released: note 62 off");

    /* hold and double_tap on one button */
    out_n = 0;
    press(BOTH);
    release(BOTH);
    run_ms(TAP_MS + 10u);
    CHECK(!strcmp(cc_values(30), "10") && !*cc_values(31) && !*cc_values(32), "both tapped: CC 30");
    out_n = 0;
    press(BOTH);
    release(BOTH);
    press(BOTH);
    release(BOTH);
    CHECK(!strcmp(cc_values(31), "10") && !*cc_values(30) && !*cc_values(32), "both twice: CC 31");
    out_n = 0;
    press(BOTH);
    run_ms(HOLD_MS + 10u);
    release(BOTH);
    run_ms(TAP_MS + 10u);
    CHECK(!strcmp(cc_values(32), "10") && !*cc_values(30) && !*cc_values(31), "both held: CC 32");

    /* button.tap_time is live */
    gm_cfg.btn_tap_time = 400;
    out_n = 0;
    press(DBL);
    release(DBL);
    run_ms(300);
    press(DBL);
    release(DBL);
    run_ms(450);
    CHECK(!strcmp(note_values(63), "10") && !*note_values(62), "tap_time 400: 300 ms apart is a double tap");
    gm_cfg.btn_tap_time = TAP_MS;
}

static void test_combos(void)
{
    /* c1 + c2 within combo_time: CC 42, not 40 or 41; it waits out combo_time (c3 could still come) */
    out_n = 0;
    press(C1);
    run_ms(10);
    press(C2);
    CHECK(out_n == 0, "c1, c2: nothing yet (c3 could make the bigger combo)");
    run_ms(COMBO_MS);
    CHECK(!strcmp(cc_values(42), "1") && !*cc_values(40) && !*cc_values(41), "c1 + c2: the combo's CC 42 (got %s)",
          cc_values(42));
    release(C2);
    CHECK(!strcmp(cc_values(42), "10"), "the first button let go: CC 42 off");
    release(C1);
    CHECK(!strcmp(cc_values(42), "10") && !*cc_values(40) && !*cc_values(41), "the second: silent");

    /* all three: the bigger combo at once, at the third press */
    out_n = 0;
    press(C3);
    press(C2);
    press(C1);
    CHECK(!strcmp(note_values(70), "1") && count(0xB0, -1, 0) == 0, "c3, c2, c1: note 70 at the last press");
    release(C1);
    release(C2);
    release(C3);
    CHECK(!strcmp(note_values(70), "10") && count(0xB0, -1, 0) == 0, "released: note 70 off, nothing else");

    /* too far apart: each its own CC, the first at combo_time */
    out_n = 0;
    press(C1);
    run_ms(COMBO_MS - 10u);
    CHECK(out_n == 0, "c1 alone: waits for combo_time");
    run_ms(20);
    CHECK(!strcmp(cc_values(40), "1"), "c1 after combo_time: its own CC 40");
    press(C2);
    run_ms(COMBO_MS + 10u);
    CHECK(!strcmp(cc_values(41), "1") && !*cc_values(42), "c2 late: its own CC 41, no combo");
    release(C1);
    release(C2);
    CHECK(!strcmp(cc_values(40), "10") && !strcmp(cc_values(41), "10"), "both let go");

    /* let go before combo_time: a tap of its own */
    out_n = 0;
    level[PIN_BTN(C1)] = 0;
    run_ms(5);
    level[PIN_BTN(C1)] = 1;
    run_ms(RELEASE_MS);
    CHECK(!strcmp(cc_values(40), "10") && !*cc_values(42), "c1 tapped: CC 40 on and off");

    /* c1 + c2 let go before combo_time: still the combo (c3 never came) */
    out_n = 0;
    press(C1);
    press(C2);
    level[PIN_BTN(C1)] = 1;
    run_ms(RELEASE_MS);
    CHECK(!strcmp(cc_values(42), "10") && !*cc_values(40), "c1 + c2, c1 let go early: CC 42 on and off");
    release(C2);
    CHECK(!strcmp(cc_values(42), "10") && !*cc_values(41), "c2 let go: silent");

    /* another button's press decides at once: c1 goes out before plain */
    out_n = 0;
    press(C1);
    press(PLAIN);
    CHECK(!strcmp(cc_values(40), "1") && !strcmp(cc_values(10), "1") && out[1] == 40, "c1, then plain: c1 first");
    release(PLAIN);
    release(C1);

    /* button.combo_time is live */
    gm_cfg.btn_combo_time = 200;
    out_n = 0;
    press(C1);
    run_ms(150);
    press(C2);
    run_ms(210);
    CHECK(!strcmp(cc_values(42), "1"), "combo_time 200: 150 ms apart is the combo");
    release(C1);
    release(C2);
    gm_cfg.btn_combo_time = COMBO_MS;
}

/* Plain pressed and let go: its CC, 10 (its own) or 11 (its twin in
 * fn_layer), on and off. */
static int plain_cc(void)
{
    out_n = 0;
    press(PLAIN);
    release(PLAIN);
    int is_own = !strcmp(cc_values(10), "10") && !*cc_values(11);
    int is_twin = !strcmp(cc_values(11), "10") && !*cc_values(10);
    return is_own ? 10 : is_twin ? 11 : -1;
}

static void test_one_shot(void)
{
    CHECK(plain_cc() == 10, "plain: its own CC 10");

    /* fn tapped: the layer for the next press only */
    out_n = 0;
    press(FN);
    release(FN);
    run_ms(20);
    CHECK(count(0xB0, -1, 0) == 0, "fn sends nothing");
    CHECK(level[PIN_FN_LED] == 1, "fn tapped: its LED lit, the layer waits for the next press");
    CHECK(plain_cc() == 11, "after fn: plain's twin, CC 11");
    run_ms(20);
    CHECK(level[PIN_FN_LED] == 0, "used: the LED off");
    CHECK(plain_cc() == 10, "and back: CC 10");

    /* fn held: the layer for every press while it is held, and off with it */
    press(FN);
    CHECK(plain_cc() == 11, "fn held: CC 11");
    CHECK(plain_cc() == 11, "still held: CC 11 again");
    release(FN);
    CHECK(plain_cc() == 10, "fn let go after a press: CC 10");

    /* fn tapped twice: on, then off */
    press(FN);
    release(FN);
    press(FN);
    release(FN);
    CHECK(plain_cc() == 10, "fn tapped twice: off, CC 10");

    /* another button's press takes it */
    press(FN);
    release(FN);
    out_n = 0;
    press(TAP_HOLD);
    release(TAP_HOLD);
    CHECK(!strcmp(cc_values(20), "10"), "after fn: tap_hold is its own");
    CHECK(plain_cc() == 10, "and it took the one-shot: plain is CC 10");

    /* a twin's release goes to the twin, even after the layer dropped */
    press(FN);
    release(FN);
    out_n = 0;
    press(PLAIN);
    press(TAP_HOLD);
    release(PLAIN);
    release(TAP_HOLD);
    CHECK(!strcmp(cc_values(11), "10") && !*cc_values(10), "plain's twin let go as CC 11 (got %s)", cc_values(11));
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

    /* a combo's note and a double tap's note, let go by a preset switch */
    out_n = 0;
    press(C1);
    press(C2);
    press(C3);
    press(DBL);
    release(DBL);
    press(DBL);
    CHECK(!strcmp(note_values(70), "1") && !strcmp(note_values(63), "1"), "note 70 (combo) and 63 (double tap) on");
    press(NOTE_HOLD);
    run_ms(HOLD_MS + 10u);
    CHECK(gm_cfg.preset_active == 4, "note_hold held: the next preset");
    CHECK(!strcmp(note_values(70), "10") && !strcmp(note_values(63), "10"), "the switch let go of notes 70 and 63");
    out_n = 0;
    release(C1);
    release(C2);
    release(C3);
    release(DBL);
    release(NOTE_HOLD);
    run_ms(TAP_MS + 50u);
    CHECK(out_n == 0, "released after the switch: silent");
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
    test_double_tap();
    test_combos();
    test_one_shot();
    test_nothing_hangs();

    printf("test_gestures: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
