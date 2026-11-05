/*
 * test_matrix.c: host test of decoder rows, row-pair key contacts, the key
 * map and switch-matrix buttons and encoders (core/test/matrix-test.yaml),
 * against a simulated 74HC138-style decoder board.
 *
 * Built with -DHW_SCAN_TEST against matrix-hw-test.yaml (the same board with
 * hardware_scan:), the scan is the platform's instead: each tick is one
 * frame, built like the STM32F1's DMA does (store each plan row's select
 * word, read the column port) and handed to gm_scan_frames().
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
#include "gm_presets.h"
#include "app_proto.h"
#include "params.h"
#include "gsx.h"

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

/* The display's messages (linked with --wrap=gm_display_message). */
void __real_gm_display_message(const char *s);
static char shown[32];
void __wrap_gm_display_message(const char *s)
{
    strncpy(shown, s, sizeof shown - 1);
    __real_gm_display_message(s);
}

/* ---- helpers ------------------------------------------------------------ */

#if GM_N_CYCLES
/* The n-th EVENT of this type for index i: its offset in out (*v its value), or -1. */
static int event_nth(int type, int i, int nth, int *v)
{
    for (int k = 0; k + GSX_OVERHEAD < out_n; k++) {
        if (out[k] != 0xF0 || out[k + 1 + GSX_OFF_CMD] != GSX_MSG_EVENT)
            continue;
        const uint8_t *p = out + k + 1 + GSX_OFF_PAYLOAD;
        if (p[0] == type && (i < 0 || (int)gsx_get_u(p + 1, 2) == i) && nth-- == 0) {
            *v = (int)gsx_get_u(p + 3, 2);
            return k;
        }
    }
    return -1;
}

static int n_events(int type, int i)
{
    int n = 0, v;
    while (event_nth(type, i, n, &v) >= 0)
        n++;
    return n;
}

/* A gemifi message from the host (USB-MIDI events on cable 0). */
static void host_sysex(uint8_t cmd, const uint8_t *p, int n)
{
    static uint8_t buf[64];
    static uint8_t seq = 20;
    gsx_tx_t t;
    seq = (uint8_t)((seq + 1) & 0x7F);
    gsx_tx_begin(&t, buf, cmd, seq);
    for (int i = 0; i < n; i++)
        gsx_put(&t, p[i]);
    int len = gsx_tx_end(&t);
    for (int k = 0; k < len; k += 3) {
        int m = len - k > 3 ? 3 : len - k;
        uint8_t ev[4] = { (uint8_t)(len - k > 3 ? 4 : 4 + m), buf[k], m > 1 ? buf[k + 1] : 0, m > 2 ? buf[k + 2] : 0 };
        gm_midi_rx_usb(ev);
    }
}

/* SET_PARAM from the host: id, index, value (u35). */
static void host_set(uint16_t id, uint16_t index, int32_t v)
{
    uint8_t q[9] = { (uint8_t)(id & 0x7F), (uint8_t)(id >> 7), (uint8_t)(index & 0x7F), (uint8_t)(index >> 7) };
    uint32_t u = (uint32_t)v;
    for (int k = 0; k < 5; k++)
        q[4 + k] = (uint8_t)((u >> (7 * k)) & 0x7F);
    host_sysex(GSX_CMD_SET_PARAM, q, 9);
}

/* GET_COMPONENT kind / index: the reply status; *r = the payload after it, *rn its length. */
static int get_component(int kind, int index, const uint8_t **r, int *rn)
{
    uint8_t q[3] = { (uint8_t)kind, (uint8_t)(index & 0x7F), (uint8_t)(index >> 7) };
    out_n = 0;
    host_sysex(GSX_CMD_GET_COMPONENT, q, 3);
    for (int i = 0; i + 1 + GSX_OFF_PAYLOAD < out_n; i++)
        if (out[i] == 0xF0 && out[i + 1 + GSX_OFF_CMD] == (GSX_CMD_GET_COMPONENT | GSX_REPLY)) {
            const uint8_t *b = out + i + 1;
            *r = b + GSX_OFF_PAYLOAD + 1;
            *rn = (b[GSX_OFF_LEN] | b[GSX_OFF_LEN + 1] << 7) - 1;
            return b[GSX_OFF_PAYLOAD];
        }
    return -1;
}
#endif

static uint32_t tick_ns;             /* scan_us, or a hardware-scan frame */
#ifdef HW_SCAN_TEST
static const gm_scan_plan_t *plan;
static uint32_t frame[GM_SCAN_MAX_ROWS / 2u];   /* 32-bit words, as the DMA ring */

static void hw_frame(uint32_t lost)
{
    uint16_t *f = (uint16_t *)frame;
    for (uint32_t r = 0; r < plan->n_rows; r++) {
        *plan->sel_reg = plan->sel_word[r];
        apply_bsrr();
        update_port_b();
        f[r] = (uint16_t)*plan->in_reg;
    }
    gm_scan_frames(f, 1, lost);
    gm_scan_tick();                  /* the scan tick keeps running: must not interfere */
}
#endif

static void ticks(uint32_t n)
{
    while (n--) {
#ifdef HW_SCAN_TEST
        hw_frame(0);
#else
        gm_scan_tick();
#endif
    }
}

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        ticks(1000000u / tick_ns);
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
    ticks(gap_us * 1000u / tick_ns);
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

/* encoder: A = output 17 col 0, B = output 17 col 1, one detent clockwise */
static void turn(void)
{
    static const uint8_t seq[5][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { 0, 0 } };
    out_n = 0;
    for (int i = 0; i < 5; i++) {
        closed[17][0] = seq[i][0];
        closed[17][1] = seq[i][1];
        run_ms(2);
    }
    run_ms(5);
}

int main(void)
{
    fast_cols = getenv("GM_TEST_FAST_COLS") != NULL;
    use_bsrr = getenv("GM_TEST_BSRR") != NULL;
    tick_ns = gm_device.scan_us * 1000u;
#ifdef HW_SCAN_TEST
    fast_cols = use_bsrr = 1;        /* what a hardware scan needs */
    printf("hardware scan (simulated DMA frames)\n");
#else
    printf("decoder select: %s\n", use_bsrr ? "one set/reset store" : "pin by pin");
    printf("column reads: %s\n", fast_cols ? "one load per row" : "per pin");
#endif
    gm_init(0x00010000u, 0);
#ifdef HW_SCAN_TEST
    plan = gm_scan_plan();
    CHECK(plan != 0, "a plan for decoder rows + one column port");
    if (!plan)
        return 1;
    /* 4 key rows, then the switch matrix's 2 (outputs 16, 17): 6, even. */
    CHECK(plan->n_rows == 6, "plan rows: %u (4 key + 2 panel)", (unsigned)plan->n_rows);
    CHECK(plan->in_mask == 0x000Fu, "column bits PB0-PB3 (got 0x%X)", (unsigned)plan->in_mask);
    CHECK(plan->row_ns == 2000 && plan->settle_ns == 1000 && plan->precharge == 1,
          "plan timing from hardware_scan:");
    tick_ns = plan->n_rows * plan->row_ns;
    CHECK(!gm_scan_hw(), "not on before the platform says so");
    gm_scan_hw_started(tick_ns * 16u / 1000u);
    CHECK(gm_scan_hw(), "on");
#endif
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

#if GM_N_MODES > 0
    /* hold mode: while chan (output 16 col 2) is held, up / down change the
     * channel instead of sending their CCs, and the keys keep playing */
    out_n = 0;
    closed[16][2] = 1;
    run_ms(20);
    CHECK(level[32] == 1, "chan held: its LED (PC0) is lit");
    CHECK(!last_msg(0xB0, &d1, &d2), "chan itself sends nothing");
    closed[16][0] = 1;
    run_ms(20);
    closed[16][0] = 0;
    run_ms(20);
    CHECK(!last_msg(0xB0, &d1, &d2), "up in the mode: no CC 20, press or release");
    press(1, 3, 1000);
    CHECK(last_msg(0x91, &d1, &d2) && d1 == 36, "channel 2 after up; keys still play");
    release(1, 3);
    out_n = 0;
    closed[16][1] = 1;                /* down twice: 2 -> 1 -> 16 */
    run_ms(20);
    closed[16][1] = 0;
    run_ms(20);
    closed[16][1] = 1;
    run_ms(20);
    closed[16][1] = 0;
    run_ms(20);
    press(1, 3, 1000);
    CHECK(last_msg(0x9F, &d1, &d2) && d1 == 36, "down from 1 wraps to 16");
    release(1, 3);
    /* up pressed in the mode (16 wraps to 1), released after it: still silent */
    out_n = 0;
    closed[16][0] = 1;
    run_ms(20);
    closed[16][2] = 0;
    run_ms(30);                     /* release debounce + an LED update */
    CHECK(level[32] == 0, "chan released: LED off");
    closed[16][0] = 0;
    run_ms(20);
    CHECK(!last_msg(0xB0, &d1, &d2), "a mode press's release is silent after the mode ends");
    /* up pressed before chan: its release still sends */
    closed[16][0] = 1;
    run_ms(20);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 20 && d2 == 127, "out of the mode: up = CC 20 again (ch 1)");
    closed[16][2] = 1;
    run_ms(20);
    out_n = 0;
    closed[16][0] = 0;
    run_ms(20);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 20 && d2 == 0, "pressed before the mode: its release is normal");
    closed[16][2] = 0;
    run_ms(20);
#endif

#if GM_LAYERS
    /* Button layers: mute (output 17 col 3) is a CC 24 toggle with colour 1
     * on its LED (PC1/PC2); Shift (16/3) + lmode (17/2) toggles the arm
     * layer, where mute is its twin: CC 25, its own state, colour 2. */
#define TAP(r, c) do { closed[r][c] = 1; run_ms(20); closed[r][c] = 0; run_ms(20); } while (0)
    out_n = 0;
    TAP(17, 3);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 24 && d2 == 127, "mute: CC 24 on (got %d %d)", d1, d2);
    CHECK(level[33] == 1 && level[34] == 0, "mute on: colour 1");
    out_n = 0;
    TAP(17, 2);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 23 && d2 == 0, "lmode alone: its own CC 23, press and release");
    CHECK(level[35] == 0, "lmode alone: no layer, arm LED off");
    closed[16][3] = 1;                /* Shift held + lmode: the arm layer */
    run_ms(20);
    out_n = 0;
    TAP(17, 2);
    CHECK(!last_msg(0xB0, &d1, &d2) || d1 != 23, "Shift + lmode: no CC 23");
    closed[16][3] = 0;
    run_ms(20);
    CHECK(level[35] == 1, "arm layer: its LED is lit");
    CHECK(level[33] == 0 && level[34] == 0, "arm layer: the twin's state (off) on the LED");
    out_n = 0;
    TAP(17, 3);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 25 && d2 == 127, "twin: CC 25 on (got %d %d)", d1, d2);
    CHECK(level[33] == 0 && level[34] == 1, "twin on: colour 2");
    /* lmode's twin (a toggle: { with: } button may have one): alone it is
     * CC 26, and the layer stays; Shift + it leaves (below) */
    out_n = 0;
    TAP(17, 2);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 26 && d2 == 0, "lmode in the layer: its twin's CC 26 (got %d)", d1);
    CHECK(level[35] == 1, "lmode's twin alone: still in the layer");
    /* the encoder's twin: CC 31, its own value; the base isn't stepped */
    turn();
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 31 && d2 == 1, "encoder in the layer: CC 31 = 1 (got %d %d)", d1, d2);
    {
        int base_sent = 0;
        for (int k = 0; k + 2 < out_n; k++)
            if (out[k] == 0xB0 && out[k + 1] == 30)
                base_sent = 1;
        CHECK(!base_sent, "the base encoder sends nothing in the layer");
    }
    closed[16][3] = 1;                /* back to the base layer */
    run_ms(20);
    TAP(17, 2);
    closed[16][3] = 0;
    run_ms(20);
    CHECK(level[35] == 0, "base layer again: arm LED off");
    CHECK(level[33] == 1 && level[34] == 0, "base layer again: mute's own state (on, colour 1)");
    out_n = 0;
    TAP(17, 3);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 24 && d2 == 0, "mute: CC 24 off; the twin's state untouched");
    CHECK(level[33] == 0 && level[34] == 0, "mute off: LED dark");
    /* a press goes to the button live when it went down, its release too */
    closed[16][3] = 1;
    run_ms(20);
    TAP(17, 2);                       /* arm layer */
    closed[16][3] = 0;
    run_ms(20);
    out_n = 0;
    closed[17][3] = 1;                /* twin pressed: off -> ... it was on: CC 25 off */
    run_ms(20);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 25 && d2 == 0, "twin again: CC 25 off");
    closed[16][3] = 1;
    run_ms(20);
    TAP(17, 2);                       /* layer off while it is held */
    closed[16][3] = 0;
    run_ms(20);
    out_n = 0;
    closed[17][3] = 0;
    run_ms(20);
    CHECK(!last_msg(0xB0, &d1, &d2) || d1 != 24, "its release isn't the base's press");
    out_n = 0;
    TAP(17, 3);
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 24 && d2 == 127, "the next press is the base's: CC 24 on");
    TAP(17, 3);
#undef TAP
#endif

    /* encoder, out of any layer: CC 30, from its own value (the twin's turns left it alone) */
    turn();
    CHECK(last_msg(0xB0, &d1, &d2) && d1 == 30 && d2 == 1, "encoder turn: CC 30 = 1 (got %d %d)", d1, d2);

#if GM_N_CYCLES
    /* Cycles (core/README.md "Cycles"): Shift (16/3) + emode (PC4) moves
     * the encs cycle to its next enabled step: OFF (2, disabled), PLAIN (3),
     * DEV (4), PAN (5); emode_led (PD0-PD2, RGB) shows the step's colour. */
#define EMODE 36                     /* PC4 */
#define ELED 48                      /* PD0..PD2: red, green, blue */
#define LED_IS(r, g, b) (level[ELED] == (r) && level[ELED + 1] == (g) && level[ELED + 2] == (b))
#define TAP(r, c) do { closed[r][c] = 1; run_ms(20); closed[r][c] = 0; run_ms(20); } while (0)
#define SHIFT_TAP_PIN(pin) do { closed[16][3] = 1; run_ms(20); level[pin] = 1; run_ms(20); level[pin] = 0; \
                                run_ms(20); closed[16][3] = 0; run_ms(20); } while (0)
#define CYCLE() SHIFT_TAP_PIN(EMODE)
#define ARM() do { closed[16][3] = 1; run_ms(20); TAP(17, 2); closed[16][3] = 0; run_ms(20); } while (0)
    {
        int v;
        CHECK(LED_IS(0, 0, 1), "power-on: the first enabled step, PLAIN: blue (OFF is disabled)");
        /* the schema: layer.* global, button.* / encoder.* the YAML's own */
        const param_def_t *p = params_find(P_BTN_ACTION);
        CHECK(p && p->count == 7 && (p->flags & PF_PRESET), "button.action: the 7 own buttons, per preset");
        p = params_find(P_LAYER_BTN_NUMBER);
        CHECK(p && p->count == 3 && !(p->flags & PF_PRESET) && p->defs[2] == 40 &&
              !strcmp(p->index_labels[2], "Mute s_dev"), "layer.button.number: 3 twins, global, mute_s_dev = 40");
        p = params_find(P_ENC_CC);
        CHECK(p && p->count == 1, "encoder.cc: the own encoder only");
        p = params_find(P_LAYER_ENC_CC);
        CHECK(p && p->count == 4 && p->min == -1 && p->defs[0] == 31 && p->defs[1] == 33 && p->defs[2] == -1 &&
              p->defs[3] == 34, "layer.encoder.cc: 4 twins, -1..127, DEV's is none (-1)");
        p = params_find(P_LAYER_ENABLED);
        CHECK(p && p->count == 6 && p->defs[2] == 0 && p->defs[3] == 1 && !strcmp(p->index_labels[3], "PLAIN") &&
              !strcmp(p->index_labels[1], "arm"), "layer.enabled: per mode, OFF off, labels name / id");

        /* debug.events 1 from the host: EVENT 8 for each mode on, every time */
        out_n = 0;
        host_set(P_EVENTS, 0, 1);
        run_ms(2);
        CHECK(n_events(EV_LAYER, -1) == 1 && event_nth(EV_LAYER, 3, 0, &v) >= 0 && v == 1,
              "events on: EVENT 8 mode 3 (PLAIN) = 1, nothing else (%d)", n_events(EV_LAYER, -1));
        out_n = 0;
        host_set(P_EVENTS, 0, 1);
        run_ms(2);
        CHECK(n_events(EV_LAYER, 3) == 1, "set to 1 again (already 1): the replay again");

        /* emode alone: its own CC 27; the cycle stays */
        out_n = 0;
        level[EMODE] = 1;
        run_ms(20);
        CHECK(last_msg(0xB0, &d1, &d2) && d1 == 27 && d2 == 127, "emode alone: CC 27");
        level[EMODE] = 0;
        run_ms(20);
        CHECK(LED_IS(0, 0, 1) && n_events(EV_LAYER, -1) == 0, "still PLAIN, no EVENT 8");

        /* Shift + emode: DEV, its name, green, EVENT 8 3 = 0 then 4 = 1 */
        out_n = 0;
        shown[0] = 0;
        CYCLE();
        int off = event_nth(EV_LAYER, 3, 0, &v), off_v = v, on = event_nth(EV_LAYER, 4, 0, &v);
        CHECK(off >= 0 && off_v == 0 && on > off && v == 1, "EVENT 8: PLAIN off, then DEV on");
        CHECK(!strcmp(shown, "DEV"), "the display: DEV (got '%s')", shown);
        CHECK(LED_IS(0, 1, 0), "emode_led green");
        CHECK(!last_msg(0xB0, &d1, &d2) || d1 != 27, "Shift + emode: no CC 27");
        /* DEV's encoder twin: cc none, an EVENT with its index (3), no CC */
        turn();
        CHECK(!last_msg(0xB0, &d1, &d2), "DEV: the encoder sends no CC");
        CHECK(event_nth(EV_ENCODER, 3, 0, &v) >= 0 && v == 1, "DEV: EVENT encoder 3 = +1");

        /* arm on as well: independent of the cycle */
        out_n = 0;
        ARM();
        CHECK(event_nth(EV_LAYER, 1, 0, &v) >= 0 && v == 1 && n_events(EV_LAYER, 4) == 0,
              "arm on: EVENT 8 1 = 1; DEV stays");
        CHECK(level[35] == 1 && LED_IS(0, 1, 0), "arm LED lit, emode_led still green");
        /* mute has twins in both: DEV's wins (a cycle step before a layer-only toggle) */
        out_n = 0;
        TAP(17, 3);
        CHECK(last_msg(0xB0, &d1, &d2) && d1 == 40 && d2 == 127, "mute: DEV's twin, CC 40 (got %d %d)", d1, d2);
        turn();
        CHECK(!last_msg(0xB0, &d1, &d2) && event_nth(EV_ENCODER, 3, 0, &v) >= 0,
              "the encoder: DEV's twin, not arm's CC 31");
        out_n = 0;
        host_set(P_EVENTS, 0, 1);
        run_ms(2);
        CHECK(n_events(EV_LAYER, -1) == 2 && n_events(EV_LAYER, 1) == 1 && n_events(EV_LAYER, 4) == 1,
              "the replay: arm and DEV");

        /* the twins' settings are global: a preset switch keeps them */
        host_set(P_LAYER_BTN_NUMBER, 2, 41);
        host_set(P_LAYER_ENC_CC, 2, 35);
        run_ms(2);
        presets_select(2);
        run_ms(50);
        CHECK(gm_cfg.btn_number[5] == 44, "preset 2: mute's own CC 44 (per preset)");
        CHECK(gm_cfg.btn_number[9] == 41 && gm_cfg.enc_cc[3] == 35, "preset 2: the twins keep 41 / 35");
        presets_select(1);
        run_ms(50);
        CHECK(gm_cfg.btn_number[5] == 24 && gm_cfg.btn_number[9] == 41 && gm_cfg.enc_cc[3] == 35,
              "preset 1: mute 24 again, the twins still 41 / 35");
        CHECK(LED_IS(0, 1, 0) && level[35] == 1, "a preset switch keeps the layers on");
        out_n = 0;
        TAP(17, 3);
        CHECK(last_msg(0xB0, &d1, &d2) && d1 == 41 && d2 == 127, "DEV's mute twin: CC 41 (toggles dropped)");
        TAP(17, 3);
        turn();
        CHECK(last_msg(0xB0, &d1, &d2) && d1 == 35 && d2 == 1, "DEV's encoder twin with a CC now: 35 = 1");
        host_set(P_LAYER_ENC_CC, 2, -1);
        run_ms(2);

        /* GET_COMPONENT's layer byte: mode index + 1 for a twin, 0 else */
        const uint8_t *r;
        int rn;
        CHECK(get_component(GSX_KIND_ENCODER, 3, &r, &rn) == GSX_OK && r[rn - 1] == 5 && r[rn - 2] == 0,
              "encoder 3 (enc_s_dev): mode 0, layer 5 (mode 4)");
        get_component(GSX_KIND_ENCODER, 0, &r, &rn);
        CHECK(r[rn - 1] == 0, "encoder 0: layer 0");
        get_component(GSX_KIND_BUTTON, 9, &r, &rn);
        CHECK(r[rn - 1] == 5 && gsx_get_u(r + rn - 3, 2) == 5, "button 9 (mute_s_dev): LED 5, layer 5");
        get_component(GSX_KIND_BUTTON, 7, &r, &rn);
        CHECK(r[rn - 1] == 2, "button 7 (mute_arm): layer 2 (mode 1)");
        get_component(GSX_KIND_LED, 5, &r, &rn);
        CHECK(r[rn - 1] == 5, "LED 5 (mute_led_s_dev): layer 5");
        get_component(GSX_KIND_LED, 3, &r, &rn);
        CHECK(r[rn - 1] == 0 && r[rn - 3] == 3, "LED 3 (emode_led): RGB, layer 0");

        /* next: PAN (yellow), then OFF is skipped: PLAIN */
        out_n = 0;
        CYCLE();
        CHECK(!strcmp(shown, "PAN") && LED_IS(1, 1, 0), "PAN: yellow (got '%s')", shown);
        CHECK(event_nth(EV_LAYER, 4, 0, &v) >= 0 && v == 0 && event_nth(EV_LAYER, 5, 0, &v) >= 0 && v == 1,
              "EVENT 8: DEV off, PAN on");
        turn();
        CHECK(last_msg(0xB0, &d1, &d2) && d1 == 34, "PAN: the encoder's CC 34");
        out_n = 0;
        TAP(17, 3);
        CHECK(last_msg(0xB0, &d1, &d2) && d1 == 25, "PAN has no mute twin: arm's, CC 25");
        TAP(17, 3);
        out_n = 0;
        ARM();
        CHECK(event_nth(EV_LAYER, 1, 0, &v) >= 0 && v == 0 && level[35] == 0 && LED_IS(1, 1, 0),
              "arm off: EVENT 8 1 = 0; PAN stays");
        CYCLE();
        CHECK(!strcmp(shown, "PLAIN") && LED_IS(0, 0, 1), "past the end: OFF skipped, PLAIN (got '%s')", shown);

        /* enable OFF: it's visited; disable every step: the first one */
        host_set(P_LAYER_ENABLED, 2, 1);
        run_ms(2);
        CYCLE();
        CYCLE();
        CYCLE();
        CHECK(!strcmp(shown, "OFF") && LED_IS(1, 0, 0), "OFF enabled: PAN -> OFF, red (got '%s')", shown);
        for (uint16_t m = 2; m <= 5; m++)
            host_set(P_LAYER_ENABLED, m, 0);
        run_ms(2);
        CYCLE();
        CHECK(!strcmp(shown, "OFF"), "every step disabled: the first (OFF)");
        for (uint16_t m = 3; m <= 5; m++)
            host_set(P_LAYER_ENABLED, m, 1);
        run_ms(2);
        CYCLE();
        CHECK(!strcmp(shown, "PLAIN"), "enabled again: PLAIN");
        /* a disabled toggle mode can't be entered: Shift + lmode is lmode's own */
        host_set(P_LAYER_ENABLED, 1, 0);
        run_ms(2);
        out_n = 0;
        ARM();
        CHECK(level[35] == 0 && n_events(EV_LAYER, 1) == 0, "arm disabled: no layer");
        {
            int cc23 = 0;
            for (int k = 0; k + 2 < out_n; k++)
                cc23 |= out[k] == 0xB0 && out[k + 1] == 23 && out[k + 2] == 127;
            CHECK(cc23, "Shift + lmode: lmode's own CC 23");
        }
        host_set(P_LAYER_ENABLED, 1, 1);
        host_set(P_EVENTS, 0, 0);
        run_ms(2);
    }
#undef TAP
#endif

#ifdef HW_SCAN_TEST
    /* Velocity resolution: the frames are 12 us, so 200 us apart is two
     * velocities (the software scan's 250 us ticks can't tell them apart). */
    press(1, 1, 3000);
    int v3000 = last_msg(0x90, &d1, &d2) ? d2 : -1;
    release(1, 1);
    press(1, 1, 3200);
    int v3200 = last_msg(0x90, &d1, &d2) ? d2 : -1;
    release(1, 1);
    CHECK(v3000 > v3200 && v3200 > 1, "3.0 ms = %d, 3.2 ms = %d: finer than a scan tick", v3000, v3200);

    /* Frames the platform lost (an overrun) still count as time. */
    out_n = 0;
    closed[2][1] = 1;
    ticks(1);
    hw_frame(3000u * 1000u / tick_ns);      /* 3 ms of lost frames */
    closed[3][1] = 1;
    run_ms(2);
    CHECK(last_msg(0x90, &d1, &d2) && d2 == v3000, "lost frames count: %d (3.0 ms = %d)", d2, v3000);
    release(1, 1);

    /* One frame stores each plan row's select once. */
    selects = 0;
    ticks(1);
    CHECK(selects == (int)plan->n_rows, "one frame: %d selects (%u rows)", selects, (unsigned)plan->n_rows);
#else
    /* the decoder only rewrites its lines when the selection changes */
    selects = 0;
    ticks(1);
    CHECK(selects > 0 && selects <= 6 * 5, "one scan pass: %d select-line writes (at most 6 rows x 5)", selects);
#endif

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
