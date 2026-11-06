/*
 * test_msg.c: host test of the MIDI message kinds (core/README.md "MIDI
 * messages") with core/test/msg-test.yaml: 14-bit CC, NRPN / RPN with the
 * null RPN, bank select, panic, MMC, SysEx with a Roland checksum, the
 * sign-magnitude encoder mode, per-control channel / min / max, and the
 * host's values coming back in (14-bit CC, NRPN).
 *
 * Buttons 0 panic, 1 MMC play, 2 program 5 bank 300 ch 3, 3 SysEx
 * gs_volume (max 100), 4 CC 64 10..100 ch 2, 5 note 50, 6 program next
 * bank 1/2. Analog 0 cc14 7, 1 nrpn14 1234, 2 rpn 2, 3 SysEx cutoff ch 9,
 * 4 CC 20 100..20 ch 5. Encoders 0 relative_sign 40, 1 cc14 3, 2 nrpn 500
 * 10..20, 3 CC 41 ch 4.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "gm_presets.h"
#include "params.h"
#include "app_proto.h"
#include "gsx.h"

#if !(GM_MSG_CC14 && GM_MSG_NRPN && GM_NULL_RPN && GM_MSG_PROGRAM && GM_MSG_PANIC && GM_MSG_MMC && \
      GM_N_SYSEX == 2 && GM_BTN_CTL && GM_ANALOG_CTL && GM_ENC_CTL && GM_BTN_BANK && GM_PRESET_BANK)
#error "test_msg.c needs the msg-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static uint8_t level[64];
static int32_t adc[64];

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin) { return level[pin]; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { return adc[pin]; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

static uint8_t out[16384];
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

/* Buttons are PB0.. (16 + i), inverted. */
static void press(int b, int down)
{
    level[16 + b] = down ? 0 : 1;
    run_ms(30);
}

/* One detent of encoder e (pins PC0 + 2e, PC1 + 2e: 32 + 2e, 33 + 2e). */
static void detent(int e, int dir)
{
    static const uint8_t seq[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    int pa = 32 + 2 * e, pb = 33 + 2 * e;
    for (int s = 1; s <= 4; s++) {
        int k = dir > 0 ? s & 3 : (4 - s) & 3;
        level[pa] = seq[k][0];
        level[pb] = seq[k][1];
        gm_scan_tick();
        gm_scan_tick();
    }
    run_ms(200);                    /* no acceleration: a pause is harmless */
}

/* The bytes sent since the last clear, without the gemifi SysEx (F0 7D). */
static int sent(uint8_t *dst, int max)
{
    int n = 0;
    for (int i = 0; i < out_n; i++) {
        if (out[i] == 0xF0 && i + 1 < out_n && out[i + 1] == 0x7D) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (n < max)
            dst[n++] = out[i];
    }
    return n;
}

static int is(const uint8_t *want, int n)
{
    uint8_t got[2048];
    int k = sent(got, (int)sizeof got);
    int ok = k == n && !memcmp(got, want, (size_t)n);
    if (!ok) {
        printf("  sent %d:", k);
        for (int i = 0; i < k && i < 64; i++)
            printf(" %02X", got[i]);
        printf("\n  want %d:", n);
        for (int i = 0; i < n; i++)
            printf(" %02X", want[i]);
        printf("\n");
    }
    out_n = 0;
    return ok;
}
#define IS(...) is((const uint8_t[]){ __VA_ARGS__ }, (int)sizeof((const uint8_t[]){ __VA_ARGS__ }))

/* The last n bytes sent (after the settling of an analog input). */
static int ends(const uint8_t *want, int n)
{
    uint8_t got[2048];
    int k = sent(got, (int)sizeof got);
    int ok = k >= n && !memcmp(got + k - n, want, (size_t)n);
    if (!ok) {
        printf("  last of %d:", k);
        for (int i = k > n ? k - n : 0; i < k; i++)
            printf(" %02X", got[i]);
        printf("\n");
    }
    out_n = 0;
    return ok;
}
#define ENDS(...) ends((const uint8_t[]){ __VA_ARGS__ }, (int)sizeof((const uint8_t[]){ __VA_ARGS__ }))

static void host(uint8_t a, uint8_t b, uint8_t c)
{
    const uint8_t m[3] = { a, b, c };
    gm_fb_msg(m, 3);
}

/* ---- the tests -------------------------------------------------------- */

int main(void)
{
    for (int i = 0; i < 64; i++)
        level[i] = 1;               /* buttons up (inverted), encoder pins high */
    for (int i = 32; i < 40; i++)
        level[i] = 0;               /* encoders at rest: A = B = 0 */
    level[1] = level[2] = 0;        /* keys up: a column reads 1 = closed */
    gm_init(0x00010000u, 0);
    run_ms(2000);
    out_n = 0;

    /* -- panic: Note Offs for what gemifi holds, then 120 / 123 / 121 ------ */
    press(5, 1);                    /* note 50 */
    level[1] = 1;                   /* key 0: note 60 */
    run_ms(10);
    CHECK(IS(0x90, 50, 100, 0x90, 60, 100), "note button and key: Note On 50, 60");
    press(0, 1);
    {
        uint8_t want[6 + 16 * 9], *w = want;
        *w++ = 0x80, *w++ = 60, *w++ = 0;
        *w++ = 0x80, *w++ = 50, *w++ = 0;
        for (int c = 0; c < 16; c++) {
            *w++ = (uint8_t)(0xB0 | c), *w++ = 120, *w++ = 0;
            *w++ = (uint8_t)(0xB0 | c), *w++ = 123, *w++ = 0;
            *w++ = (uint8_t)(0xB0 | c), *w++ = 121, *w++ = 0;
        }
        CHECK(is(want, (int)(w - want)), "panic: Note Off 50, then 120, 123, 121 on 16 channels");
    }
    press(0, 0);
    press(5, 0);
    level[1] = 0;
    run_ms(10);
    CHECK(is((const uint8_t *)"", 0), "panic: the held key's and button's releases send nothing");

    /* -- MMC ---------------------------------------------------------------- */
    press(1, 1);
    press(1, 0);
    CHECK(IS(0xF0, 0x7F, 0x10, 0x06, 0x02, 0xF7), "MMC play to device 0x10");

    /* -- program with bank, on the button's channel ------------------------- */
    press(2, 1);
    press(2, 0);
    CHECK(IS(0xB2, 0, 2, 0xB2, 32, 44, 0xC2, 5), "program 5, bank 300 (MSB 2, LSB 44), channel 3");
    press(6, 1);
    press(6, 0);
    CHECK(IS(0xB0, 0, 1, 0xB0, 32, 2, 0xC0, 6), "program next goes on from 5, with its bank 1 / 2");

    /* -- SysEx: value max (100) on press, min (0) on release, checksummed --- */
    press(3, 1);
    CHECK(IS(0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x04, 100, 0x58, 0xF7), "GS volume 100, checksum 58");
    press(3, 0);
    CHECK(IS(0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x04, 0, 0x3C, 0xF7), "GS volume 0 on release, checksum 3C");

    /* -- a CC button with min / max and a channel --------------------------- */
    press(4, 1);
    CHECK(IS(0xB1, 64, 100), "CC 64 on channel 2: max 100");
    press(4, 0);
    CHECK(IS(0xB1, 64, 10), "... and min 10 on release");

    /* -- analog inputs -------------------------------------------------------- */
    adc[3] = 2048;                  /* a_cc14 (PA3): 2048 -> 8193 = 64 << 7 | 1 */
    run_ms(400);
    CHECK(ENDS(0xB0, 7, 64, 0xB0, 39, 1), "cc14: MSB on 7, LSB on 39");
    adc[4] = 4095;                  /* a_nrpn: 1234 = 9 << 7 | 82, value 16383 */
    run_ms(400);
    CHECK(ENDS(0xB0, 99, 9, 0xB0, 98, 82, 0xB0, 6, 127, 0xB0, 38, 127, 0xB0, 101, 127, 0xB0, 100, 127),
          "nrpn14: 99 / 98, 6 / 38, then the null RPN");
    adc[5] = 4095;                  /* a_rpn: RPN 2, 7-bit */
    run_ms(400);
    CHECK(ENDS(0xB0, 101, 0, 0xB0, 100, 2, 0xB0, 6, 127, 0xB0, 101, 127, 0xB0, 100, 127), "rpn: 101 / 100, 6");
    adc[6] = 4095;                  /* a_sx: cutoff, 14-bit, channel 9 */
    run_ms(400);
    CHECK(ENDS(0xF0, 0x43, 8, 0x7F, 0x7F, 0x7F, 0xF7), "SysEx: ch 8, msb 7F, lsb 7F");
    adc[7] = 4095;                  /* a_rng: 100..20, channel 5: the top is 20 */
    run_ms(400);
    CHECK(ENDS(0xB4, 20, 20), "CC 20 on channel 5, min 100 max 20: the top sends 20");
    adc[7] = 0;
    run_ms(400);
    CHECK(ENDS(0xB4, 20, 100), "... and the bottom 100");

    /* -- encoders ------------------------------------------------------------- */
    detent(0, 1);
    CHECK(IS(0xB0, 40, 0x01), "sign-magnitude: +1 = 01");
    detent(0, -1);
    CHECK(IS(0xB0, 40, 0x41), "sign-magnitude: -1 = 41");
    detent(1, 1);
    CHECK(IS(0xB0, 3, 0, 0xB0, 35, 63), "cc14 encoder: 1 detent = 63 (1/256 of the range)");
    detent(2, 1);
    CHECK(IS(0xB0, 99, 3, 0xB0, 98, 116, 0xB0, 6, 11, 0xB0, 101, 127, 0xB0, 100, 127),
          "nrpn encoder 500, 10..20: the first detent sends 11");
    detent(3, 1);
    CHECK(IS(0xB3, 41, 1), "CC 41 on channel 4");

    /* -- the host's values ---------------------------------------------------- */
    host(0xB3, 41, 100);            /* channel 4: e_ch follows */
    host(0xB0, 41, 10);             /* channel 1: not e_ch's */
    detent(3, 1);
    CHECK(IS(0xB3, 41, 101), "the host's CC on the encoder's channel: 100, then 101");
    host(0xB0, 3, 16);              /* e_cc14: MSB 16, LSB 5 = 2053 */
    host(0xB0, 35, 5);
    detent(1, 1);
    CHECK(IS(0xB0, 3, 16, 0xB0, 35, 68), "cc14 from the host: 2053 + 63 = 2116 (16 / 68)");
    host(0xB0, 99, 3);              /* NRPN 500 = 15 */
    host(0xB0, 98, 116);
    host(0xB0, 6, 15);
    detent(2, 1);
    CHECK(IS(0xB0, 99, 3, 0xB0, 98, 116, 0xB0, 6, 16, 0xB0, 101, 127, 0xB0, 100, 127),
          "NRPN from the host: 15, then 16");
    gm_encoder_host(1, 16383);      /* FB_VALUE: the top */
    detent(1, -1);
    CHECK(IS(0xB0, 3, 127, 0xB0, 35, 0x40), "FB_VALUE on a cc14 encoder: 16383 - 63 = 16320");

    /* -- presets: bank before the program change ------------------------------- */
    presets_select(2);
    CHECK(IS(0xB0, 0, 1, 0xB0, 32, 3, 0xC0, 7), "preset 2: bank 1 / 3, program 7");
    adc[7] = 4095;                  /* a_rng is nrpn 300 in preset 2: still 100..20 on channel 5 */
    run_ms(400);
    CHECK(ENDS(0xB4, 99, 2, 0xB4, 98, 44, 0xB4, 6, 20, 0xB4, 101, 127, 0xB4, 100, 127), "preset 2: a_rng as NRPN 300");
    presets_select(1);
    CHECK(IS(0xB0, 0, 0, 0xB0, 32, 2, 0xC0, 1), "preset 1: bank 2, program 1");

    /* -- the params ------------------------------------------------------------ */
    CHECK(params_set(P_BTN_ACTION, 0, ACT_MCU, 1) == GSX_BAD_VALUE, "no mcu: the Mackie Control action is refused");
    CHECK(params_set(P_BTN_ACTION, 0, ACT_SYSEX, 1) == GSX_OK, "the SysEx action is there");
    CHECK(params_set(P_ANALOG_CC, 0, 16383, 1) == GSX_OK, "with NRPN, analog.cc goes to 16383");
    CHECK(params_set(P_ENC_MODE, 0, ENC_RPN14, 1) == GSX_OK && params_set(P_ENC_MODE, 0, ENC_RPN14 + 1, 1) == GSX_BAD_VALUE,
          "encoder.mode: up to rpn14");
    CHECK(params_set(P_PRESET_BANK, 0, 16383, 1) == GSX_OK && params_set(P_PRESET_BANK, 0, 16384, 1) == GSX_BAD_VALUE,
          "preset.bank: -1..16383");

    printf("test_msg: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
