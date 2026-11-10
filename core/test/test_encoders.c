/*
 * test_encoders.c: host test of encoder acceleration (core/README.md
 * "Encoder acceleration") with core/test/encoders-test.yaml, on fake time:
 * each detent is scanned at once, then the main loop runs once per ms, so
 * the time between two detents is exactly the gap a test asks for.
 *
 * Encoders: 0 med (medium: slow 80ms, fast 10ms, max 8, quadratic), 1 lin
 * (slow 100ms, fast 20ms, max 4, linear), 2 plain (off), 3 pot (V-Pot 1,
 * medium), 4 rel (high: slow 100ms, fast 8ms, max 16, quadratic); 5 and 6
 * are med's twins in the cycle's ZYN (cc: none) and CC (CC 40) steps.
 *
 * The expected steps follow from the curve: the first detent after a pause
 * is 1 step, then the smoothed interval (avg = (avg + t) / 2, in 1/16 ms)
 * gives x = (slow - avg) / (slow - fast), and 1 + (max - 1) * x (or x^2)
 * steps per detent, the fraction carried to the next detent.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_presets.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "app_proto.h"
#include "gsx.h"

#if !(GM_ENC_ACCEL && GM_MCU && GM_N_CYCLES && GM_ENC_TWINS == 2 && GM_N_ENCODERS == 7)
#error "test_encoders.c needs the encoders-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { MED, LIN, PLAIN, POT, REL, MED_ZYN, MED_CC };

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
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

static uint8_t out[16384], out_cable[16384];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    if (out_n + (int)n < (int)sizeof out) {
        memcpy(out + out_n, m, n);
        memset(out_cable + out_n, cable, n);
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

static void scan(void)
{
    gm_scan_tick();
    gm_scan_tick();
}

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

/* One detent of encoder e (pins a = PB4 + 2e, b = PB5 + 2e), scanned at
 * once, no main loop in between: dir + = clockwise. */
static void detent(int e, int dir)
{
    static const uint8_t seq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    int pa = 20 + 2 * e, pb = 21 + 2 * e;
    for (int s = 1; s <= 4; s++) {
        int k = dir > 0 ? s & 3 : (4 - s) & 3;
        level[pa] = seq[k][0];
        level[pb] = seq[k][1];
        scan();
    }
}

/* n detents of encoder e, gap ms apart (each read by the next pass). */
static void turn(int e, int n, uint32_t gap)
{
    for (int k = 0; k < (n < 0 ? -n : n); k++) {
        detent(e, n);
        run_ms(gap);
    }
}

/* The CC values sent for CC num on the main port, in order: count, into v. */
static int ccs(int num, int *v, int max)
{
    int n = 0;
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == 0xB0 && out_cable[i] == 0 && out[i + 1] == num && n < max)
            v[n++] = out[i + 2];
    }
    return n;
}

/* The V-Pot 1 CCs (B0 10 on the DAW port) as signed deltas. */
static int vpots(int *v, int max)
{
    int n = 0;
    for (int i = 0; i + 2 < out_n; i++)
        if (out[i] == 0xB0 && out_cable[i] == 1 && out[i + 1] == 0x10 && n < max)
            v[n++] = out[i + 2] & 0x40 ? -(out[i + 2] & 0x0F) : out[i + 2] & 0x0F;
    return n;
}

/* EVENT encoder deltas for encoder index i (14-bit two's complement). */
static int events(int i, int *v, int max)
{
    int n = 0;
    for (int k = 0; k + GSX_OVERHEAD < out_n; k++) {
        if (out[k] != 0xF0 || out[k + 1] != 0x7D || out[k + 1 + GSX_OFF_CMD] != GSX_MSG_EVENT)
            continue;
        const uint8_t *p = out + k + 1 + GSX_OFF_PAYLOAD;
        if (p[0] == EV_ENCODER && (int)gsx_get_u(p + 1, 2) == i && n < max) {
            int x = (int)gsx_get_u(p + 3, 2);
            v[n++] = x & 0x2000 ? x - 0x4000 : x;
        }
    }
    return n;
}

/* Absolute values -> steps per detent (the first from `from`). */
static void diffs(const int *v, int n, int from, int *d)
{
    for (int k = 0; k < n; k++) {
        d[k] = v[k] - from;
        from = v[k];
    }
}

static int same(const int *a, const int *b, int n)
{
    return !memcmp(a, b, (size_t)n * sizeof *a);
}

static void show(const char *what, const int *v, int n)
{
    printf("  %s:", what);
    for (int k = 0; k < n; k++)
        printf(" %d", v[k]);
    printf("\n");
}

/* Start encoder e's absolute value at v and let it rest (a pause resets it). */
static void rest(int e, int v)
{
    gm_encoder_host((uint32_t)e, (uint32_t)v << 7);     /* FB_VALUE: 14-bit */
    run_ms(300);
    out_n = 0;
}

static void press(int pin, int down)
{
    level[pin] = down ? 0 : 1;
    run_ms(30);
}

/* Shift + emode: the cycle's next step. */
static void cycle_next(void)
{
    press(16, 1);
    press(17, 1);
    press(17, 0);
    press(16, 0);
}

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    int v[256], d[256];
    level[16] = level[17] = 1;          /* buttons released */
    gm_init(0x01000000u, 0);
    CHECK(presets_rec_fits(), "a record of either scope fits presets.c's rec[]");
    run_ms(2000);
    gm_cfg.events = 1;                  /* debug.events: the EVENTs too */

    CHECK(gm_enc_accel[MED].max == 8 && gm_enc_accel[MED].slow_ms == 80 && gm_enc_accel[PLAIN].max == 1 &&
          gm_enc_accel[REL].max == 16, "the table: med medium, plain off, rel high");

    /* -- slow turns: exactly 1 per detent ----------------------------- */
    rest(MED, 0);
    turn(MED, 20, 100);
    {
        int n = ccs(30, v, 256);
        diffs(v, n, 0, d);
        int ok = n == 20;
        for (int k = 0; k < n; k++)
            ok &= d[k] == 1;
        CHECK(ok, "100 ms apart: CC 30 = 1..20, one per detent (%d CCs, last %d)", n, n ? v[n - 1] : -1);
    }
    rest(MED, 0);
    turn(MED, 20, 80);                  /* exactly slow: still a pause each time */
    CHECK(ccs(30, v, 256) == 20 && v[19] == 20, "80 ms apart (= slow): 1 per detent (last %d)", v[19]);
    rest(MED, 0);
    turn(MED, 24, 79);                  /* just under slow: x^2 too small to add a step */
    CHECK(ccs(30, v, 256) == 24 && v[23] == 24, "79 ms apart: still 24 for 24 detents (last %d)", v[23]);

    /* -- a fast flick scales up along the curve ------------------------ */
    rest(MED, 0);
    turn(MED, 12, 8);
    {
        static const int want[12] = { 1, 2, 5, 7, 8, 7, 8, 8, 8, 8, 8, 8 };
        int n = ccs(30, v, 256);
        diffs(v, n, 0, d);
        CHECK(n == 12 && same(d, want, 12) && v[11] == 78, "12 detents 8 ms apart: 1 2 5 7 8 7 8.. = 78 (got %d)",
              n ? v[n - 1] : -1);
        if (!(n == 12 && same(d, want, 12)))
            show("steps", d, n);
        n = events(MED, v, 256);
        CHECK(n == 12 && same(v, want, 12), "EVENT encoder 0: the same accelerated steps");
    }
    rest(MED, 0);
    turn(MED, 24, 8);                   /* a quick full turn of a 24-detent encoder: the whole range */
    CHECK(ccs(30, v, 256) > 0 && v[ccs(30, v, 256) - 1] == 127, "24 detents 8 ms apart: 0 -> 127");
    rest(MED, 0);
    turn(MED, 24, 20);                  /* a brisk turn: about a full sweep (132 unclamped) */
    {
        int n = ccs(30, v, 256);
        CHECK(n == 24 && v[23] == 127 && v[20] < 127, "24 detents 20 ms apart: 127 near the end (v[20] %d)", v[20]);
    }
    rest(MED, 100);                     /* and down again, the same steps */
    turn(MED, -12, 8);
    CHECK(ccs(30, v, 256) == 12 && v[11] == 100 - 78, "12 detents back, 8 ms apart: 100 -> 22 (got %d)", v[11]);

    /* -- several detents in one pass ------------------------------------ */
    rest(MED, 0);
    turn(MED, 6, 8);                    /* 1 2 5 7 8 7 = 30 */
    detent(MED, 1);                     /* three at once, 8 ms after the last */
    detent(MED, 1);
    detent(MED, 1);
    run_ms(1);
    {
        int n = ccs(30, v, 256);
        CHECK(n == 7 && v[5] == 30 && v[6] - v[5] == 24, "3 detents in one pass at speed: 3 x 8 = 24 (got %d)",
              n == 7 ? v[6] - v[5] : -1);
    }

    /* -- fractional speeds: linear, x = 1/2 -> 2.5 steps per detent ---- */
    rest(LIN, 0);
    turn(LIN, 20, 60);
    {
        static const int want[20] = { 1, 1, 2, 3, 2, 3, 2, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2 };
        int n = ccs(31, v, 256);
        diffs(v, n, 0, d);
        CHECK(n == 20 && same(d, want, 20) && v[19] == 46, "lin, 60 ms apart: 2 and 3 in turn, 46 for 20 (got %d)",
              n ? v[n - 1] : -1);
        if (!(n == 20 && same(d, want, 20)))
            show("steps", d, n);
        int s = 0;
        for (int k = 10; k < 20; k++)
            s += d[k];
        CHECK(s == 25, "lin: 2.5 a detent once settled (10 detents: %d)", s);
    }

    /* -- reversal: 1 step back, then it builds again -------------------- */
    rest(MED, 64);
    turn(MED, 10, 8);                   /* 1 2 5 7 8 7 8 8 8 8 = 62 -> 126 */
    turn(MED, -1, 8);
    turn(MED, -1, 8);
    turn(MED, -1, 8);
    {
        int n = ccs(30, v, 256);
        diffs(v, n, 64, d);
        CHECK(n == 13 && v[9] == 126 && d[10] == -1 && d[11] == -2 && d[12] == -5,
              "reversal at speed: -1, then -2, -5 (got %d %d %d)", d[10], d[11], d[12]);
        n = events(MED, v, 256);
        CHECK(n == 13 && v[10] == -1, "EVENT after the reversal: -1");
    }
    /* the carried fraction goes too: back and forth, slowly, is exact */
    rest(MED, 64);
    turn(MED, 5, 8);                    /* 1 2 5 7 8 = 23 -> 87, with a fraction carried */
    turn(MED, -1, 100);
    turn(MED, 1, 100);
    turn(MED, -1, 100);
    {
        int n = ccs(30, v, 256);
        CHECK(n == 8 && v[4] == 87 && v[5] == 86 && v[6] == 87 && v[7] == 86, "back and forth: 86 87 86 (got %d %d %d)",
              v[5], v[6], v[7]);
    }

    /* -- an idle gap resets ------------------------------------------- */
    rest(MED, 0);
    turn(MED, 8, 8);                    /* 1 2 5 7 8 7 8 8 = 46 */
    run_ms(150);                        /* a pause longer than slow */
    turn(MED, 3, 8);
    {
        int n = ccs(30, v, 256);
        diffs(v, n, 0, d);
        CHECK(n == 11 && v[7] == 46 && d[8] == 1 && d[9] == 2 && d[10] == 5,
              "after a pause: 1, 2, 5 again (got %d %d %d)", d[8], d[9], d[10]);
    }

    /* -- off: one per detent at any speed (and several in one pass) ------ */
    rest(PLAIN, 0);
    turn(PLAIN, 30, 2);
    detent(PLAIN, 1);
    detent(PLAIN, 1);
    detent(PLAIN, 1);
    run_ms(1);
    {
        int n = ccs(32, v, 256);
        diffs(v, n, 0, d);
        int ok = n == 31;
        for (int k = 0; k < 30 && k < n; k++)
            ok &= d[k] == 1;
        CHECK(ok && v[30] == 33, "off, 2 ms apart: 1 per detent, 3 at once = 3 (%d CCs, last %d)", n, n ? v[n - 1] : -1);
        n = events(PLAIN, v, 256);
        CHECK(n == 31 && v[0] == 1 && v[30] == 3, "off: EVENTs carry the detents");
    }

    /* -- Mackie Control V-Pot: accelerated, at most 15 a message -------- */
    out_n = 0;
    run_ms(300);
    turn(POT, 12, 8);
    {
        static const int want[12] = { 1, 2, 5, 7, 8, 7, 8, 8, 8, 8, 8, 8 };
        int n = vpots(v, 256);
        CHECK(n == 12 && same(v, want, 12), "V-Pot 1, 8 ms apart: 1 2 5 7 8 ..");
        if (!(n == 12 && same(v, want, 12)))
            show("vpot", v, n);
    }
    out_n = 0;
    detent(POT, 1);
    detent(POT, 1);
    detent(POT, 1);
    run_ms(1);                          /* 3 x 8 = 24: sent as 15 */
    CHECK(vpots(v, 256) == 1 && v[0] == 15, "V-Pot: 24 steps at once -> +15 (got %d)", v[0]);
    run_ms(300);
    out_n = 0;
    turn(POT, -3, 100);
    CHECK(vpots(v, 256) == 3 && v[0] == -1 && v[1] == -1 && v[2] == -1, "V-Pot, slow: -1 each");

    /* -- relative modes: accelerated, still clamped to -64..63 ---------- */
    gm_cfg.enc_mode[REL] = ENC_REL_TWOS;
    run_ms(300);
    out_n = 0;
    turn(REL, 12, 8);                   /* high: 1 4 10 12 14 15 16 16 15 16 16 16 */
    {
        static const int want[12] = { 1, 4, 10, 12, 14, 15, 16, 16, 15, 16, 16, 16 };
        int n = ccs(33, v, 256);
        CHECK(n == 12 && same(v, want, 12), "relative (two's complement), 8 ms apart: 1 4 10 12 ..");
        if (!(n == 12 && same(v, want, 12)))
            show("rel", v, n);
    }
    out_n = 0;
    for (int k = 0; k < 6; k++)
        detent(REL, 1);
    run_ms(1);                          /* 6 x 16 = 96: clamped to 63 */
    CHECK(ccs(33, v, 256) == 1 && v[0] == 63, "relative: 96 at once -> 63 (got %d)", v[0]);
    run_ms(300);
    turn(REL, -12, 8);
    out_n = 0;
    for (int k = 0; k < 6; k++)
        detent(REL, -1);
    run_ms(1);                          /* -96: clamped to -64 = 0x40 */
    CHECK(ccs(33, v, 256) == 1 && v[0] == 0x40, "relative: -96 at once -> -64 (0x40, got 0x%02X)", v[0]);
    gm_cfg.enc_mode[REL] = ENC_REL_OFFSET;
    run_ms(300);
    turn(REL, 12, 8);
    out_n = 0;
    for (int k = 0; k < 6; k++)
        detent(REL, 1);
    run_ms(1);
    CHECK(ccs(33, v, 256) == 1 && v[0] == 127, "relative offset: 96 at once -> 64 + 63 (got %d)", v[0]);
    run_ms(300);
    out_n = 0;
    turn(REL, -2, 100);
    CHECK(ccs(33, v, 256) == 2 && v[0] == 63 && v[1] == 63, "relative offset, slow: 63 each");

    /* -- the cycle: one step per Shift + emode press, whatever the knob --- */
    rest(MED, 0);
    turn(MED, 12, 8);                   /* fast turns just before ... */
    out_n = 0;
    cycle_next();                       /* ... don't make the press count more: PLAIN -> ZYN */
    turn(MED, 12, 8);
    {
        static const int want[12] = { 1, 2, 5, 7, 8, 7, 8, 8, 8, 8, 8, 8 };
        int n = events(MED_ZYN, v, 256);
        CHECK(n == 12 && same(v, want, 12) && ccs(30, d, 256) == 0,
              "ZYN: the twin's EVENTs (cc: none), the base's acceleration; no CC 30");
    }
    /* the layer changes in the middle of a fast turn: the twin carries on at speed */
    press(16, 1);                       /* Shift held */
    run_ms(100);
    out_n = 0;
    turn(MED, 6, 8);                    /* ZYN: 1 2 5 7 8 7 */
    level[17] = 0;                      /* emode down between two detents: ZYN -> CC */
    turn(MED, 3, 8);                    /* read after the buttons: CC 40, at speed: 8 8 8 */
    CHECK(events(MED_ZYN, v, 256) == 6 && v[5] == 7, "ZYN: 6 EVENTs, the last +7");
    CHECK(ccs(40, v, 256) == 3 && v[0] == 8 && v[1] == 16 && v[2] == 24,
          "CC: the twin's own value at the base's speed, 8 16 24 (got %d %d %d)", v[0], v[1], v[2]);
    press(17, 0);
    press(16, 0);
    run_ms(100);
    out_n = 0;
    turn(MED, 3, 100);                  /* still CC 40: one press, one step; slow: 1 each */
    CHECK(ccs(40, v, 256) == 3 && v[2] == 27 && ccs(30, d, 256) == 0, "one press moved the cycle one step");
    out_n = 0;
    cycle_next();                       /* CC -> PLAIN */
    turn(MED, 1, 8);
    CHECK(ccs(30, v, 256) == 1 && ccs(40, d, 256) == 0, "PLAIN again: CC 30");

    printf("test_encoders: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
