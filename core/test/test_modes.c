/*
 * test_modes.c: host test of modes on a device without a key matrix (#117)
 * with core/test/modes-test.yaml: a layer-only toggle (fx), a Shift + button
 * toggle with a layer (arm), a hold mode with buttons: (chan), a toggle with
 * buttons: (menu) and a cycle (encs). Layer twins send their own CCs, the
 * cycle steps round its enabled steps.
 *
 * Buttons 0 b_a CC 20, 1 mute CC 24 toggle, 2 shift CC 22, 3 lmode CC 23,
 * 4 fx, 5 chan, 6 emode, 7 up CC 25, 8 menu (PB0.., inverted). LEDs
 * mute_led PD4, arm_led PD3, emode_led PD0-PD2 (RGB). Encoder enc CC 30 on
 * PC0 / PC1.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"

#if GM_HAS_MATRIX || GM_N_MODES != 7 || GM_N_CYCLES != 1 || GM_BTN_TWINS != 3 || GM_ENC_TWINS != 2
#error "test_modes.c needs the modes-test.yaml config"
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
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

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

void __real_gm_display_message(const char *s);
static char shown[32];
void __wrap_gm_display_message(const char *s)
{
    strncpy(shown, s, sizeof shown - 1);
    __real_gm_display_message(s);
}

/* ---- helpers ---------------------------------------------------------- */

enum { B_A, MUTE, SHIFT, LMODE, FX, CHAN, EMODE, UP, MENU };
#define PIN_BTN(b) (16 + (b))       /* PB0.. */
#define PIN_ENC_A 32                /* PC0, PC1 */
#define PIN_ARM_LED 51              /* PD3 */
#define PIN_MUTE_LED 52             /* PD4 */
#define PIN_RGB 48                  /* PD0..PD2: red, green, blue */
#define RGB_IS(r, g, b) (level[PIN_RGB] == (r) && level[PIN_RGB + 1] == (g) && level[PIN_RGB + 2] == (b))

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        gm_poll();
    }
}

static void hold(int b, int down)
{
    level[PIN_BTN(b)] = down ? 0 : 1;   /* inverted */
    run_ms(30);
}

static void tap(int b)
{
    hold(b, 1);
    hold(b, 0);
}

/* Shift held + b. */
static void shift_tap(int b)
{
    hold(SHIFT, 1);
    tap(b);
    hold(SHIFT, 0);
}

/* One detent clockwise. */
static void detent(void)
{
    static const uint8_t seq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int s = 1; s <= 4; s++) {
        level[PIN_ENC_A] = seq[s & 3][0];
        level[PIN_ENC_A + 1] = seq[s & 3][1];
        gm_scan_tick();
        gm_scan_tick();
    }
    run_ms(200);
}

/* Channel-voice messages sent since the last clear (SysEx skipped): how
 * many of status st with data byte 1 = d1 (-1: any); *last the last one's
 * second data byte. */
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

/* Any CC on channel 1 sent since the last clear. */
static int any_cc(void)
{
    return count(0xB0, -1, 0);
}

/* ---- the tests -------------------------------------------------------- */

int main(void)
{
    int v = -1;
    for (int i = 0; i < 64; i++)
        level[i] = 1;               /* buttons up (inverted) */
    level[PIN_ENC_A] = level[PIN_ENC_A + 1] = 0;
    gm_init(0x00010000u, 0);
    run_ms(2000);
    out_n = 0;

    /* -- no mode: the buttons' own CCs ------------------------------------ */
    tap(B_A);
    CHECK(count(0xB0, 20, &v) == 2 && v == 0, "b_a: CC 20 press and release");
    out_n = 0;
    tap(MUTE);
    CHECK(count(0xB0, 24, &v) == 1 && v == 127, "mute: CC 24 on");
    CHECK(level[PIN_MUTE_LED] == 1, "mute on: its LED");
    out_n = 0;
    detent();
    CHECK(count(0xB0, 30, &v) == 1 && v == 1, "encoder: CC 30 = 1 (got %d)", v);
    CHECK(RGB_IS(1, 0, 0), "power-on: the cycle's first step ONE, red");

    /* -- fx: a layer-only toggle ----------------------------------------- */
    out_n = 0;
    tap(FX);
    CHECK(!any_cc(), "fx sends nothing (it only toggles the layer)");
    tap(B_A);
    CHECK(count(0xB0, 40, &v) == 2 && count(0xB0, 20, 0) == 0, "fx layer: b_a's twin sends CC 40, not 20");
    out_n = 0;
    tap(FX);
    tap(B_A);
    CHECK(count(0xB0, 20, 0) == 2 && count(0xB0, 40, 0) == 0, "fx off: b_a is CC 20 again");

    /* -- arm: Shift + lmode, mute and the encoder have twins ------------- */
    out_n = 0;
    tap(LMODE);
    CHECK(count(0xB0, 23, 0) == 2 && level[PIN_ARM_LED] == 0, "lmode alone: its own CC 23, no layer");
    shift_tap(LMODE);
    CHECK(level[PIN_ARM_LED] == 1, "Shift + lmode: the arm layer, its LED lit");
    CHECK(level[PIN_MUTE_LED] == 0, "arm layer: the LED shows the twin's state (off)");
    out_n = 0;
    tap(MUTE);
    CHECK(count(0xB0, 26, &v) == 1 && v == 127 && count(0xB0, 24, 0) == 0, "arm layer: mute's twin CC 26 on");
    out_n = 0;
    detent();
    CHECK(count(0xB0, 31, &v) == 1 && v == 1 && count(0xB0, 30, 0) == 0,
          "arm layer: the encoder's twin CC 31 = 1, from its own value");
    /* fx on together with arm: both layers live */
    tap(FX);
    out_n = 0;
    tap(B_A);
    tap(MUTE);
    CHECK(count(0xB0, 40, 0) == 2 && count(0xB0, 26, &v) == 1 && v == 0, "fx and arm on together");
    tap(FX);
    shift_tap(LMODE);
    CHECK(level[PIN_ARM_LED] == 0, "Shift + lmode again: arm off");
    CHECK(level[PIN_MUTE_LED] == 1, "the base's state (on) back on the LED");
    out_n = 0;
    tap(MUTE);
    CHECK(count(0xB0, 24, &v) == 1 && v == 0, "mute: CC 24 off, its state kept through the layer");

    /* -- chan: hold, up raises the channel ------------------------------- */
    out_n = 0;
    hold(CHAN, 1);
    tap(UP);
    CHECK(!any_cc() && count(0xB1, -1, 0) == 0, "chan held: up sends no CC 25");
    hold(CHAN, 0);
    tap(B_A);
    CHECK(count(0xB1, 20, 0) == 2, "channel 2 after up");
    out_n = 0;
    tap(UP);
    CHECK(count(0xB1, 25, 0) == 2, "chan released: up is CC 25 again (on channel 2)");

    /* -- menu: a toggle with buttons:, up lowers the channel ------------- */
    out_n = 0;
    tap(MENU);
    tap(UP);
    tap(MENU);
    tap(UP);
    CHECK(count(0xB0, 25, 0) == 2 && count(0xB1, 25, 0) == 0, "menu + up: back to channel 1; menu off: CC 25");

    /* -- encs: the cycle ---------------------------------------------------- */
    out_n = 0;
    tap(EMODE);
    CHECK(!any_cc(), "emode sends nothing");
    CHECK(RGB_IS(0, 1, 0) && !strcmp(shown, "TWO"), "emode: TWO, green, its name shown (got %s)", shown);
    detent();
    CHECK(count(0xB0, 33, &v) == 1 && v == 1 && count(0xB0, 30, 0) == 0, "TWO: the encoder's twin CC 33");
    tap(EMODE);
    CHECK(RGB_IS(0, 0, 1) && !strcmp(shown, "THREE"), "emode: THREE, blue");
    out_n = 0;
    detent();
    tap(MUTE);
    CHECK(count(0xB0, 30, &v) == 1 && v == 2, "THREE: the encoder is its own again, CC 30 = 2 (got %d)", v);
    CHECK(count(0xB0, 27, &v) == 1 && v == 127 && count(0xB0, 24, 0) == 0, "THREE: mute's twin CC 27");
    tap(EMODE);
    CHECK(RGB_IS(1, 0, 0) && !strcmp(shown, "ONE"), "emode: round to ONE, red");
    /* a disabled step is skipped (layer.enabled 0: mode 5, s_two) */
    gm_cfg.layer_enabled[5] = 0;
    tap(EMODE);
    CHECK(RGB_IS(0, 0, 1), "TWO disabled: ONE -> THREE");
    tap(EMODE);
    CHECK(RGB_IS(1, 0, 0), "THREE -> ONE");
    gm_cfg.layer_enabled[5] = 1;
    out_n = 0;
    detent();
    CHECK(count(0xB0, 30, &v) == 1 && v == 3, "ONE: the encoder's CC 30 = 3");

    printf("test_modes: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
