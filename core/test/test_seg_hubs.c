/*
 * test_seg_hubs.c: host test of the MAX7219 and TM1637 hubs (vpin.c) and
 * the 7-segment displays on them, with the device core/test/seg-hubs-test.yaml:
 * a chain of two MAX7219s (a 3-digit preset_number display on chip 0, an LED
 * on chip 1) and a 4-digit TM1637 (the last_message display).
 *
 * The simulated HAL decodes both buses from the pin writes:
 *   - MAX7219: CS low, 16-bit words MSB first on the CLK rising edge, CS
 *     high; with n words shifted in, chip c takes word n - 1 - c. Each frame
 *     is logged as "<reg>:<chip 0 data>,<chip 1 data>|";
 *   - TM1637: start (DIO falls while CLK is high), bytes LSB first on the
 *     CLK rising edge, every 9th clock the ACK (DIO must be low: the chip
 *     pulls it low, so the driver must not drive it high), stop (DIO rises
 *     while CLK is high). Each command is logged as its bytes, "40|C0 3D|".
 * Timing is checked on a virtual clock in ns, moved by hal_delay_us and by
 * each hal_cycles() call (10 ns; hal_ns_to_cycles(ns) = ns, or 0 with
 * GM_TEST_NO_CYCLES=1: no cycle counter, the MAX7219 phases on
 * hal_delay_us).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_presets.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "gm_display.h"
#include "gm_feedback.h"
#include "gm_vpin.h"

#if !defined(GM_TM1628_RAM) || GM_TM1628_RAM != 20
#error "test_seg_hubs.c needs the seg-hubs-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PIN_MX_CS  4            /* PA4 */
#define PIN_MX_CLK 5            /* PA5 */
#define PIN_MX_DIN 7            /* PA7 */
#define PIN_P1     16           /* PB0 */
#define PIN_P2     17           /* PB1 */
#define PIN_BTN    18           /* PB2 */
#define PIN_T_CLK  22           /* PB6 */
#define PIN_T_DIO  23           /* PB7 */

static uint32_t now_ms;
static uint8_t level[64];
static uint8_t mode[64];
static int in_tick, tick_writes;
static uint64_t t_ns;
#define MIN(m, v) do { uint64_t v_ = (v); if (v_ < (m)) (m) = v_; } while (0)

static char log_mx[8192], log_t[4096];
static size_t len_mx, len_t;

static void logf_(char *buf, size_t *len, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 < cap) {
        memcpy(buf + *len, s, n + 1);
        *len += n;
    }
}

static void logs_clear(void)
{
    len_mx = len_t = 0;
    log_mx[0] = log_t[0] = 0;
}

/* ---- the MAX7219 chain ------------------------------------------------- */

static uint8_t mx_reg[2][16];
static int mx_active, mx_bits, mx_odd, mx_frames;
static uint16_t mx_words[8];
static uint64_t mx_t_rise, mx_t_fall, mx_t_cs;
static uint64_t mx_min_high = ~0ull, mx_min_low = ~0ull, mx_min_cs_high = ~0ull;

static void mx_pin(uint16_t pin, int was, int l)
{
    if (pin == PIN_MX_CS && was && !l) {
        mx_active = 1;
        mx_bits = 0;
        memset(mx_words, 0, sizeof mx_words);
        if (mx_t_cs)
            MIN(mx_min_cs_high, t_ns - mx_t_cs);
    } else if (pin == PIN_MX_CS && !was && l && mx_active) {
        mx_active = 0;
        mx_t_cs = t_ns;
        if (mx_bits % 16)
            mx_odd++;
        int nw = mx_bits / 16;
        char s[64];
        int reg = nw ? (mx_words[nw - 1] >> 8) & 15 : 0;
        uint8_t d[2] = { 0, 0 };
        for (int c = 0; c < 2 && c < nw; c++) {
            uint16_t w = mx_words[nw - 1 - c];
            d[c] = (uint8_t)w;
            if ((w >> 8) & 15)
                mx_reg[c][(w >> 8) & 15] = (uint8_t)w;
        }
        snprintf(s, sizeof s, "%X:%02X,%02X|", reg, d[0], d[1]);
        logf_(log_mx, &len_mx, sizeof log_mx, s);
        mx_frames++;
    } else if (pin == PIN_MX_CLK && !was && l) {
        if (mx_active && mx_bits < 16 * 8) {
            mx_words[mx_bits / 16] = (uint16_t)(mx_words[mx_bits / 16] << 1 | level[PIN_MX_DIN]);
            mx_bits++;
        }
        if (mx_t_fall)
            MIN(mx_min_low, t_ns - mx_t_fall);
        mx_t_rise = t_ns;
    } else if (pin == PIN_MX_CLK && was && !l) {
        if (mx_t_rise)
            MIN(mx_min_high, t_ns - mx_t_rise);
        mx_t_fall = t_ns;
    }
}

/* ---- the TM1637 -------------------------------------------------------- */

static uint8_t t_ram[6];
static int t_ctl = -1, t_auto = -1, t_in, t_bits, t_n, t_ack_fights, t_odd, t_cmds;
static uint8_t t_byte, t_buf[16];
static uint64_t t_edge, t_dio, t_min_half = ~0ull, t_min_setup = ~0ull;

static void t_end(void)
{
    char s[64];
    size_t k = 0;
    t_cmds++;
    if (t_bits % 9 != 1)            /* 9 clocks a byte, then the stop's own CLK rise */
        t_odd++;
    for (int i = 0; i < t_n && k + 4 < sizeof s; i++)
        k += (size_t)snprintf(s + k, sizeof s - k, i ? " %02X" : "%02X", t_buf[i]);
    s[k++] = '|';
    s[k] = 0;
    logf_(log_t, &len_t, sizeof log_t, s);
    if (!t_n)
        return;
    uint8_t c = t_buf[0];
    if ((c & 0xC0) == 0x40) {
        t_auto = !(c & 4);
    } else if ((c & 0xC0) == 0x80) {
        t_ctl = c;
    } else if ((c & 0xC0) == 0xC0) {
        int a = c & 7;
        for (int i = 1; i < t_n; i++) {
            if (a < 6)
                t_ram[a] = t_buf[i];
            if (t_auto)
                a++;
        }
    }
}

static void t_pin(uint16_t pin, int was, int l)
{
    if (pin == PIN_T_CLK) {
        if (t_edge)
            MIN(t_min_half, t_ns - t_edge);
        t_edge = t_ns;
        if (l && t_in)
            MIN(t_min_setup, t_ns - t_dio);
    } else if (pin == PIN_T_DIO) {
        t_dio = t_ns;
    }
    if (pin == PIN_T_DIO && level[PIN_T_CLK] && was && !l) {
        t_in = 1;                   /* start */
        t_bits = t_n = 0;
        t_byte = 0;
    } else if (pin == PIN_T_DIO && level[PIN_T_CLK] && !was && l && t_in) {
        t_in = 0;                   /* stop */
        t_end();
    } else if (pin == PIN_T_CLK && !was && l && t_in) {
        int b = t_bits % 9;
        if (b < 8) {
            t_byte |= (uint8_t)(level[PIN_T_DIO] << b);
        } else {
            if (level[PIN_T_DIO])
                t_ack_fights++;     /* driven high while the chip pulls low */
            if (t_n < (int)sizeof t_buf)
                t_buf[t_n++] = t_byte;
            t_byte = 0;
        }
        t_bits++;
    }
}

/* ---- the HAL ------------------------------------------------------------ */

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { t_ns += 1000ull * us; }
uint32_t hal_cycles(void) { return (uint32_t)(t_ns += 10); }
static int no_cycles;
uint32_t hal_ns_to_cycles(uint32_t ns) { return no_cycles ? 0 : ns; }
void hal_pin_mode(uint16_t pin, uint8_t m) { mode[pin] = m; }

void hal_pin_write(uint16_t pin, int l)
{
    l = l != 0;
    int was = level[pin];
    if (in_tick && (pin == PIN_MX_CLK || pin == PIN_MX_CS || pin == PIN_MX_DIN ||
                    pin == PIN_T_CLK || pin == PIN_T_DIO))
        tick_writes++;
    level[pin] = (uint8_t)l;
    t_ns += 20;                     /* a pin write's own time */
    if (was != l) {
        mx_pin(pin, was, l);
        t_pin(pin, was, l);
    }
}

int hal_pin_read(uint16_t pin) { return level[pin]; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n) { (void)cable; (void)m; (void)n; }
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        in_tick = 1;
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        in_tick = 0;
        now_ms++;
        gm_poll();
    }
}

static void press(uint16_t pin)
{
    level[pin] = 0;
    run_ms(30);
    level[pin] = 1;
    run_ms(30);
}

/* MAX7219 digit register value of a 7-segment glyph (DP D7, A D6 .. G D0). */
static uint8_t mx_of(char c)
{
    uint8_t g = gm_seg7_glyph(c), r = g & 0x80u;
    for (int s = 0; s < 7; s++)
        if ((g >> s) & 1u)
            r |= (uint8_t)(0x40u >> s);
    return r;
}

static int tm_shows(const char *t)
{
    for (int i = 0; i < 4; i++)
        if (t_ram[i] != gm_seg7_glyph(t[i]))
            return 0;
    return 1;
}

int main(void)
{
    no_cycles = getenv("GM_TEST_NO_CYCLES") != NULL;
    printf("MAX7219 phases timed by %s\n", no_cycles ? "hal_delay_us" : "hal_cycles");
    level[PIN_MX_CS] = 1;
    level[PIN_T_CLK] = level[PIN_T_DIO] = 1;
    level[PIN_P1] = level[PIN_P2] = level[PIN_BTN] = 1;   /* released (inverted, pull-up) */

    gm_init(0x01000000u, 0);
    CHECK(mode[PIN_MX_CLK] == HAL_OUT && mode[PIN_MX_DIN] == HAL_OUT && mode[PIN_MX_CS] == HAL_OUT &&
          mode[PIN_T_CLK] == HAL_OUT && mode[PIN_T_DIO] == HAL_OUT, "control pins are outputs");
    CHECK(level[PIN_MX_CS] && !level[PIN_MX_CLK] && level[PIN_T_CLK] && level[PIN_T_DIO],
          "idle: MAX7219 CS high, CLK low; TM1637 both high");

    /* the first write: set-up, every digit, then on */
    CHECK(strncmp(log_mx, "F:00,00|9:00,00|B:02,02|A:08,08|1:", 34) == 0, "MAX7219 set-up: %s", log_mx);
    const char *on = strstr(log_mx, "C:01,01|");
    CHECK(on != NULL, "then on: %s", log_mx);
    int digits = 0;
    for (const char *q = strstr(log_mx, "A:08,08|") + 8; q && on && q < on; q = strchr(q, '|') + 1)
        digits++;
    CHECK(digits == 8, "all 8 digits written before it is turned on (%d)", digits);
    CHECK(strncmp(log_t, "40|C0 ", 6) == 0 && strstr(log_t, "|8A|") != NULL,
          "TM1637: 40, the RAM from C0, then 8A (on, intensity 2): %s", log_t);
    CHECK(mx_odd == 0 && t_odd == 0, "whole words and bytes (%d, %d)", mx_odd, t_odd);

    /* after the lamp test: the preset number on chip 0, "gemifi" on the TM1637 */
    run_ms(400);
    CHECK(mx_reg[0][1] == 0 && mx_reg[0][2] == 0 && mx_reg[0][3] == mx_of('1'),
          "preset 1, right-aligned (%02X %02X %02X)", mx_reg[0][1], mx_reg[0][2], mx_reg[0][3]);
    CHECK(mx_reg[1][1] == 0, "the LED on chip 1 is off after the lamp test (%02X)", mx_reg[1][1]);
    CHECK(tm_shows("GEMI"), "TM1637: GEMI (%02X %02X %02X %02X)", t_ram[0], t_ram[1], t_ram[2], t_ram[3]);
    CHECK(t_ctl == 0x8A && t_auto == 1, "TM1637 on at intensity 2, auto-increment");

    /* a preset switch: only the changed digit's frame */
    logs_clear();
    mx_frames = 0;
    press(PIN_P2);
    CHECK(mx_reg[0][3] == mx_of('2') && mx_reg[0][1] == 0 && mx_reg[0][2] == 0, "preset 2 (%02X)", mx_reg[0][3]);
    char want[32];
    snprintf(want, sizeof want, "3:%02X,00|", mx_of('2'));
    CHECK(strcmp(log_mx, want) == 0, "one frame, digit 3 of both chips: %s", log_mx);
    CHECK(tm_shows("LEAD"), "TM1637: the preset's name (%02X %02X %02X %02X)", t_ram[0], t_ram[1], t_ram[2], t_ram[3]);
    /* one grid a pass within the budget: 0x40, C0|a and its byte; E stays */
    snprintf(want, sizeof want, "40|C0 %02X|40|C2 %02X|40|C3 %02X|", gm_seg7_glyph('L'), gm_seg7_glyph('A'),
             gm_seg7_glyph('D'));
    CHECK(strcmp(log_t, want) == 0, "each changed grid on its own: %s", log_t);

    /* an LED on the chain's second chip */
    logs_clear();
    press(PIN_BTN);
    CHECK(mx_reg[1][1] == 0x40, "LED on: chip 1, digit 0, segment A = D6 (%02X)", mx_reg[1][1]);
    CHECK(strcmp(log_mx, "1:00,40|") == 0, "one frame: %s", log_mx);

    /* FB_TEXT on display 1 (the TM1637) */
    uint8_t txt[2] = { '4', '2' };
    gm_host_on_text(1, 0, 0, txt, 2);
    run_ms(40);
    CHECK(t_ram[0] == gm_seg7_glyph('4') && t_ram[1] == gm_seg7_glyph('2') && t_ram[2] == 0 && t_ram[3] == 0,
          "host text 42 (%02X %02X %02X %02X)", t_ram[0], t_ram[1], t_ram[2], t_ram[3]);

    /* the buses */
    CHECK(tick_writes == 0, "no MAX7219 / TM1637 pin written from the scan tick (%d)", tick_writes);
    CHECK(t_ack_fights == 0, "DIO low through every ACK clock (%d driven high)", t_ack_fights);
    CHECK(mx_min_high >= 50 && mx_min_low >= 50 && mx_min_cs_high >= 50,
          "MAX7219 CLK high %llu ns, low %llu ns, CS high %llu ns (>= 50)",
          (unsigned long long)mx_min_high, (unsigned long long)mx_min_low, (unsigned long long)mx_min_cs_high);
    CHECK(t_min_half >= 3000, "TM1637 CLK high and low >= 3 us (%llu ns)", (unsigned long long)t_min_half);
    CHECK(t_min_setup >= 3000, "TM1637 DIO set >= 3 us before CLK rises (%llu ns)", (unsigned long long)t_min_setup);

    printf("test_seg_hubs: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
