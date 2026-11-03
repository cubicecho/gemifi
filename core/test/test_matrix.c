/*
 * test_matrix.c: host test of decoder rows, row-pair key contacts, the key
 * map and switch-matrix buttons and encoders (core/test/matrix-test.yaml),
 * against a simulated 74HC138-style decoder board.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated board ---------------------------------------------------
 * Address PA2..PA4 (pins 2..4), enables PA5/PA6. Group 0: PA5=1 PA6=0,
 * group 1: PA5=0 PA6=1, group 2: both 0. Columns PB0..PB3 (pins 16..19),
 * pulled up, pulled low by a closed switch on the selected row. */
static uint32_t now_ms;
static uint8_t level[64];
static uint8_t closed[24][4];        /* [decoder output][column] */
static int selects;                  /* address/enable writes, to see the cache work */

uint32_t hal_millis(void) { return now_ms; }
static void apply_bsrr(void);
static void update_port_b(void);
/* The scan waits here between selecting a row and reading it: the simulated
 * pins catch up with a set/reset store, as the real port does at once. */
void hal_delay_us(uint32_t us)
{
    (void)us;
    apply_bsrr();
    update_port_b();
}
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l)
{
    if (pin >= 2 && pin <= 6)
        selects++;
    level[pin] = (uint8_t)l;
    update_port_b();
}

static int selected_row(void)
{
    int addr = level[2] | level[3] << 1 | level[4] << 2;
    int g = level[5] && !level[6] ? 0 : !level[5] && level[6] ? 1 : !level[5] && !level[6] ? 2 : -1;
    return g < 0 ? -1 : g * 8 + addr;
}

/* Port A set/reset register for the one-store decoder select (hal_pin_bsrr),
 * applied to the pin levels before the next read. GM_TEST_BSRR=1 turns it on. */
static int use_bsrr;
static volatile uint32_t bsrr_a;
volatile uint32_t *hal_pin_bsrr(uint16_t pin, uint32_t *bit)
{
    if (!use_bsrr || pin >= 16) {
        *bit = 0;
        return 0;
    }
    *bit = pin;
    return &bsrr_a;
}

static void apply_bsrr(void)
{
    uint32_t w = bsrr_a;
    if (!w)
        return;
    bsrr_a = 0;
    selects++;
    for (int b = 0; b < 16; b++) {
        if ((w >> (b + 16)) & 1u)
            level[b] = 0;
        if ((w >> b) & 1u)
            level[b] = 1;
    }
}

int hal_pin_read(uint16_t pin)
{
    apply_bsrr();
    if (pin >= 16 && pin < 20) {
        int r = selected_row();
        return r >= 0 && closed[r][pin - 16] ? 0 : 1;
    }
    return level[pin];
}
/* Port B input register for the one-load column reads, recomputed on every
 * pin write (the selected row follows the address/enable pins).
 * GM_TEST_FAST_COLS=1 turns it on, so both column paths are tested. */
static int fast_cols;
static volatile uint32_t port_b;
static void update_port_b(void)
{
    uint32_t v = 0;
    for (int b = 0; b < 16; b++)
        if (hal_pin_read((uint16_t)(16 + b)))
            v |= 1u << b;
    port_b = v;
}
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask)
{
    if (!fast_cols || pin < 16 || pin >= 32) {
        *mask = 0;
        return 0;
    }
    *mask = 1u << (pin & 15u);
    return &port_b;
}
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

static uint8_t out[4096];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    (void)cable;
    if (out_n + (int)n < (int)sizeof out) {
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

/* ---- helpers ------------------------------------------------------------ */

static void ticks(uint32_t n)
{
    while (n--)
        gm_scan_tick();
}

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        ticks(1000u / gm_device.scan_us);
        now_ms++;
        gm_poll();
    }
}

/* The last 3-byte message with this status in the output (SysEx skipped). */
static int last_msg(uint8_t status, int *d1, int *d2)
{
    int found = 0;
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == status) {
            *d1 = out[i + 1];
            *d2 = out[i + 2];
            found = 1;
        }
    }
    return found;
}

/* Key at row pair p, column c: first contact (row 2p) closes, then after
 * gap_us the second (row 2p+1). */
static void press(int p, int c, uint32_t gap_us)
{
    out_n = 0;
    closed[2 * p][c] = 1;
    ticks(gap_us / gm_device.scan_us);
    closed[2 * p + 1][c] = 1;
    run_ms(2);
}

static void release(int p, int c)
{
    out_n = 0;
    closed[2 * p][c] = 0;
    closed[2 * p + 1][c] = 0;
    run_ms(2);
}

int main(void)
{
    fast_cols = getenv("GM_TEST_FAST_COLS") != NULL;
    use_bsrr = getenv("GM_TEST_BSRR") != NULL;
    printf("decoder select: %s\n", use_bsrr ? "one set/reset store" : "pin by pin");
    printf("column reads: %s\n", fast_cols ? "one load per row" : "per pin");
    gm_init(0x00010000u, 0);
    run_ms(2000);
    int d1, d2;

    /* key map: row pair 0, column 0 = key 7 (note 43); pair 1, column 3 = key 0 (note 36) */
    press(0, 0, 1000);
    CHECK(last_msg(0x90, &d1, &d2) && d1 == 43 && d2 > 1, "pair 0 col 0 = key 7 = note 43 (got %d %d)", d1, d2);
    release(0, 0);
    CHECK(last_msg(0x80, &d1, &d2) && d1 == 43, "note off 43");
    press(1, 3, 1000);
    CHECK(last_msg(0x90, &d1, &d2) && d1 == 36, "pair 1 col 3 = key 0 = note 36 (got %d)", d1);
    release(1, 3);

    /* velocity from the time between the two rows of a pair */
    press(1, 1, 500);
    int fast = last_msg(0x90, &d1, &d2) ? d2 : -1;
    CHECK(d1 == 38, "pair 1 col 1 = key 2 = note 38 (got %d)", d1);
    release(1, 1);
    press(1, 1, 60000);
    int slow = last_msg(0x90, &d1, &d2) ? d2 : -1;
    release(1, 1);
    CHECK(fast > slow && slow > 0 && fast == 127, "velocity: 0.5 ms = %d (127), 60 ms = %d (lower)", fast, slow);

    /* one contact alone plays nothing */
    out_n = 0;
    closed[0][2] = 1;
    run_ms(5);
    CHECK(!last_msg(0x90, &d1, &d2), "first contact alone: no note");
    closed[0][2] = 0;
    run_ms(2);

    /* switch-matrix buttons on decoder outputs 16/17, same columns */
    out_n = 0;
    closed[16][0] = 1;
    run_ms(20);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 20 && d2 == 127, "up (output 16, col 0) = CC 20 127");
    closed[16][0] = 0;
    run_ms(20);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 20 && d2 == 0, "up released = CC 20 0");
    out_n = 0;
    closed[16][1] = 1;
    run_ms(20);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 21 && d2 == 127, "down (output 16, col 1) = CC 21 127");
    closed[16][1] = 0;
    run_ms(20);
    out_n = 0;
    closed[3][0] = 1;                /* a key contact (row 3) is not a button */
    run_ms(20);
    CHECK(!last_msg(0xB0, &d1, &d2), "key rows don't reach the switch matrix");
    closed[3][0] = 0;
    run_ms(5);

    /* encoder: A = output 17 col 0, B = output 17 col 1, one detent clockwise */
    out_n = 0;
    static const uint8_t seq[5][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { 0, 0 } };
    for (int i = 0; i < 5; i++) {
        closed[17][0] = seq[i][0];
        closed[17][1] = seq[i][1];
        run_ms(2);
    }
    run_ms(5);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 30, "encoder turn: CC 30 (got %d %d)", d1, d2);

    /* the decoder only rewrites its lines when the selection changes */
    selects = 0;
    ticks(1);
    CHECK(selects > 0 && selects <= 6 * 5, "one scan pass: %d select-line writes (at most 6 rows x 5)", selects);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
