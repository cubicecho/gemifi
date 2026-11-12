/*
 * test_pads.c: host test of the velocity pads, with the device
 * core/test/pads-test.yaml: 16 pads on two muxes that share their select
 * lines with a fader mux, one inverted pad on an ADC pin, a pad layer (twins
 * of pads 1 and 2) and a fader with touch: movement. Simulated
 * HAL: pins, a mux-aware ADC, MIDI out; RAM config store.
 *
 * Built with -DHW_ADC_TEST against pads-hw-test.yaml (the same board with
 * hardware_scan: { analog: true }), the readings come as the platform's ADC
 * sweeps instead (gm_adc_sweep, 5 a millisecond), built from the same
 * simulated mux channels.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_vpin.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_presets.h"
#include "app_proto.h"
#include "params.h"
#include "gsx.h"

#if GM_N_PADS != 17
#error "test_pads.c needs the pads-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

#define PC0 32u                     /* stm32f1 pin numbers: port * 16 + bit */
static uint32_t now_ms;
static uint8_t level[64];
static int32_t mux_a[8], mux_b[8], fader_mux[8], direct = 4095;   /* direct: inverted, 4095 = rest */
static int settles;

static uint32_t sel(void) { return level[PC0] | level[PC0 + 1] << 1 | level[PC0 + 2] << 2; }

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { if (us == 10) settles++; }
/* The XY pad (Code 61 pins, PB10 for Y-): a plate driven + high and - low
 * puts the touch point's position on the other, floating plate. Untouched,
 * the floating plate reads XY_FLOAT. Every read checks the drive is right
 * (xy_bad), and no pin of both plates is ever driven at once (xy_short). */
#define XP 17u                      /* PB1 */
#define XM 37u                      /* PC5: Y is read here */
#define YP 36u                      /* PC4: X is read here */
#define YM 26u                      /* PB10 */
#define XY_FLOAT 30
static uint8_t mode[64];
static int xy_touch, xy_bad, xy_short, xy_reads;
static int32_t xy_x, xy_y;
static int driven(uint16_t hi, uint16_t lo) { return mode[hi] == HAL_OUT && level[hi] && mode[lo] == HAL_OUT && !level[lo]; }
static int plate_out(uint16_t a, uint16_t b) { return mode[a] == HAL_OUT || mode[b] == HAL_OUT; }
void hal_pin_mode(uint16_t pin, uint8_t m)
{
    mode[pin] = m;
    if (plate_out(XP, XM) && plate_out(YP, YM))
        xy_short++;
}
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin) { return level[pin]; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
#ifdef HW_ADC_TEST
static int adc_reads;
/* The mux select lines' set/reset register, which the hardware plan needs
 * (the platform stores the step words there; sweep() sets the levels). */
static volatile uint32_t bsrr_c;
volatile uint32_t *hal_pin_bsrr(uint16_t pin, uint32_t *bit)
{
    *bit = pin & 15u;
    return pin >= 32u && pin < 48u ? &bsrr_c : 0;
}
#endif
int32_t hal_adc_read(uint16_t pin)
{
#ifdef HW_ADC_TEST
    adc_reads++;
#endif
    if (pin == YP || pin == XM) {
#ifdef HW_ADC_TEST
        adc_reads--;                /* the XY pad is read in software either way */
#endif
        xy_reads++;
        int x = pin == YP;
        uint16_t other = x ? YM : XP;
        if (mode[pin] != HAL_ANALOG || mode[other] != HAL_IN || !(x ? driven(XP, XM) : driven(YP, YM))) {
            xy_bad++;
            return 2000;
        }
        return xy_touch ? (x ? xy_x : xy_y) : XY_FLOAT;
    }
    switch (pin) {
    case 0: return mux_a[sel()];
    case 1: return mux_b[sel()];
    case 2: return direct;
    case 7: return fader_mux[sel()];
    default: return -1;
    }
}
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

static uint8_t out[16384];
static int out_n;
void hal_midi_send(const uint8_t *m, uint32_t n) { if (out_n + (int)n < (int)sizeof out) { memcpy(out + out_n, m, n); out_n += (int)n; } }
void hal_midi_send_cable(uint8_t c, const uint8_t *m, uint32_t n) { (void)c; hal_midi_send(m, n); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* ---- helpers ---------------------------------------------------------- */

#ifdef HW_ADC_TEST
#define SWEEPS_PER_MS 5u            /* the Code 61's: about 200 us a sweep */
static const gm_adc_plan_t *aplan;

/* One sweep: every plan pin at every mux step, as the DMA lays it out. */
static void sweep(void)
{
    uint16_t v[GM_ADC_MAX_STEPS * GM_ADC_MAX_PINS];
    for (uint32_t st = 0; st < aplan->n_steps; st++) {
        for (uint32_t k = 0; k < aplan->n_pins; k++) {
            level[PC0] = st & 1u;
            level[PC0 + 1] = (st >> 1) & 1u;
            level[PC0 + 2] = (st >> 2) & 1u;
            int32_t x = hal_adc_read(aplan->pin[k]);
            adc_reads--;
            v[st * aplan->n_pins + k] = (uint16_t)(x < 0 ? 0 : x);
        }
    }
    gm_adc_sweep(v);
}
#endif

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
#ifdef HW_ADC_TEST
        for (uint32_t k = 0; k < SWEEPS_PER_MS; k++)
            sweep();
#endif
        now_ms++;
        gm_poll();
    }
}

/* The n-th channel message with this status (and data 1 if d1 >= 0): its
 * offset, or -1. */
static int find_nth(uint8_t status, int d1, int nth)
{
    for (int i = 0; i < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if (out[i] == status && (d1 < 0 || (i + 1 < out_n && out[i + 1] == d1)) && nth-- == 0)
            return i;
    }
    return -1;
}

static int find(uint8_t status, int d1) { return find_nth(status, d1, 0); }

static int count(uint8_t status_hi)
{
    int n = 0;
    for (int i = 0; i < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if ((out[i] & 0xF0) == status_hi)
            n++;
    }
    return n;
}

static int has_text(const char *s)
{
    size_t n = strlen(s);
    for (int i = 0; i + (int)n <= out_n; i++)
        if (memcmp(out + i, s, n) == 0)
            return 1;
    return 0;
}

/* The n-th EVENT of this type for index i: its offset (*v its value), or -1. */
static int event_nth(int type, int i, int nth, int *v)
{
    for (int k = 0; k + GSX_OVERHEAD < out_n; k++) {
        if (out[k] != 0xF0 || out[k + 1 + GSX_OFF_CMD] != GSX_MSG_EVENT)
            continue;
        const uint8_t *p = out + k + 1 + GSX_OFF_PAYLOAD;
        if (p[0] == type && (int)gsx_get_u(p + 1, 2) == i && nth-- == 0) {
            *v = (int)gsx_get_u(p + 3, 2);
            return k;
        }
    }
    return -1;
}

/* The value of the last EVENT of this type for index i, or -1. */
static int last_event(int type, int i)
{
    int v = -1, x;
    for (int n = 0; event_nth(type, i, n, &x) >= 0; n++)
        v = x;
    return v;
}

/* The value of the last pad EVENT (type 7) for pad i, or -1. */
static int pad_event(int i) { return last_event(EV_PAD, i); }

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

/* An lstring at *p: compare with want, move past it. */
static int lstr_is(const uint8_t **p, const char *want)
{
    int n = (*p)[0], ok = n == (int)strlen(want) && memcmp(*p + 1, want, (size_t)n) == 0;
    *p += 1 + n;
    return ok;
}

#define GRID_BTN 30u                /* PB14: toggles the grid layer */
#define PAD1_LED 31u                /* PB15 */
static void grid_tap(void)
{
    level[GRID_BTN] = 1;
    run_ms(20);
    level[GRID_BTN] = 0;
    run_ms(30);
}

static void set_param(uint16_t id, int32_t v)
{
    CHECK(params_set(id, 0, v, 1) == GSX_OK, "set param %u = %d", id, v);
}

/* A strike on mux_a channel ch: the readings, one per ms. */
static void strike(int32_t *chan, const int32_t *vals, int n)
{
    for (int k = 0; k < n; k++) {
        *chan = vals[k];
        run_ms(1);
    }
}

int main(void)
{
    mux_b[7] = 3000;                    /* pad 16 pressed at power-up */
    gm_init(0x00010000u, 0);
    CHECK(presets_rec_fits(), "a record of either scope fits presets.c's rec[]");
#ifdef HW_ADC_TEST
    printf("hardware analog scan (simulated ADC sweeps)\n");
    aplan = gm_adc_plan();
    CHECK(aplan != 0, "a plan: three muxes on the same select lines, one ADC pin");
    if (!aplan)
        return 1;
    CHECK(aplan->n_steps == 8 && aplan->n_pins == 4 && aplan->pin[0] == 7 && aplan->pin[1] == 0 &&
          aplan->pin[2] == 1 && aplan->pin[3] == 2, "plan: 8 steps x PA7, PA0, PA1, PA2 (%u x %u)",
          (unsigned)aplan->n_steps, (unsigned)aplan->n_pins);
    CHECK(aplan->settle_ns == 10000, "settle = the muxes' delay (%u ns)", (unsigned)aplan->settle_ns);
    CHECK(!gm_adc_hw(), "off until the platform starts it");
    gm_adc_hw_started();
    CHECK(gm_adc_hw(), "on");
    /* before the first sweep there is no reading yet, not a 0 (a wheel would
     * take 0 as its rest and hold a bend) */
    fader_mux[3] = 2000;
    CHECK(vpin_analog(gm_device.analogs[0].pin) == -1, "no sweep yet: no reading (%d)",
          (int)vpin_analog(gm_device.analogs[0].pin));
    sweep();
    CHECK(vpin_analog(gm_device.analogs[0].pin) == 2000, "after the first sweep: the reading (%d)",
          (int)vpin_analog(gm_device.analogs[0].pin));
    fader_mux[3] = 0;
    adc_reads = 0;
#endif
    out_n = 0;
    run_ms(2000);
    CHECK(count(0x90) == 0 && count(0x80) == 0, "nothing at rest, pad 16 held at start-up stays silent");
    CHECK(has_text("Pad 16") && has_text("reads above pad.release"), "boot log names pad 16");
    CHECK(!has_text("Pad 15") && !has_text("Direct"), "only pad 16");
    mux_b[7] = 0;
    run_ms(30);
    CHECK(count(0x80) == 0, "letting it go sends nothing");

    /* params exist with the YAML's defaults */
    CHECK(gm_cfg.pad_channel == 10 && gm_cfg.pad_threshold == 200 && gm_cfg.pad_release == 100 &&
          gm_cfg.pad_full == 3000 && gm_cfg.pad_window == 3 && gm_cfg.pad_retrigger == 20 &&
          gm_cfg.pad_note[16] == 60, "pad params from pad_settings:");

    /* a hit: the peak over the 3 ms window gives the velocity */
    gm_cfg.events = 1;
    out_n = 0;
    {
        const int32_t v[] = { 500, 1500, 2500 };
        strike(&mux_a[0], v, 3);
    }
    CHECK(count(0x90) == 0, "no note during the window");
    mux_a[0] = 1800;
    run_ms(1);
    int i = find(0x99, 36);
    CHECK(i >= 0 && out[i + 2] == 104, "Note On 36 on channel 10, velocity 104 from peak 2500 (got %d)",
          i >= 0 ? out[i + 2] : -1);
    CHECK(pad_event(0) == -1, "a pad's own hit: its note, no EVENT (%d)", pad_event(0));
    run_ms(50);
    CHECK(count(0x80) == 0 && count(0x90) == 1, "held above release: one note, no off");
    mux_a[0] = 150;
    run_ms(5);
    CHECK(count(0x80) == 0, "between release and threshold: still held");
    mux_a[0] = 50;
    run_ms(1);
    CHECK(find(0x89, 36) >= 0, "below release: Note Off");
    CHECK(pad_event(0) == -1, "no EVENT on release either");

    /* retrigger guard: 20 ms after the release */
    out_n = 0;
    mux_a[0] = 2000;
    run_ms(10);
    CHECK(count(0x90) == 0, "within 20 ms of the release: no hit");
    mux_a[0] = 0;
    run_ms(15);
    mux_a[0] = 2000;
    run_ms(4);
    CHECK(count(0x90) == 1, "after the guard: a hit");
    mux_a[0] = 0;
    run_ms(25);

    /* a short hit, let go within the window: on and off at once, peak kept */
    out_n = 0;
    mux_a[1] = 1000;
    run_ms(1);
    mux_a[1] = 0;
    run_ms(1);
    i = find(0x99, 37);
    CHECK(i >= 0 && out[i + 2] == 1 + 126 * 800 / 2800 && find(0x89, 37) > i,
          "short hit: Note On 37 velocity %d, then Note Off (got %d)", 1 + 126 * 800 / 2800, i >= 0 ? out[i + 2] : -1);
    run_ms(25);

    /* the full scale and above: 127; the other mux */
    out_n = 0;
    mux_b[5] = 4000;
    run_ms(4);
    i = find(0x99, 49);
    CHECK(i >= 0 && out[i + 2] == 127 && count(0x90) == 1, "pad 14 (mux_b 5): note 49 at 127, nothing else");
    mux_b[5] = 0;
    run_ms(25);

    /* inverted ADC pin: reading = 4095 - raw */
    out_n = 0;
    direct = 4095 - 1600;
    run_ms(4);
    i = find(0x99, 60);
    CHECK(i >= 0 && out[i + 2] == 1 + 126 * 1400 / 2800, "direct pad, inverted: velocity %d (got %d)",
          1 + 126 * 1400 / 2800, i >= 0 ? out[i + 2] : -1);
    direct = 4095;
    run_ms(25);

    /* shared select lines: the fader still reads its own channel */
    out_n = 0;
    for (int k = 0; k < 8; k++)
        fader_mux[k] = k == 3 ? 4095 : 0;
    run_ms(150);                        /* one analog read per 2 ms in turn (3 inputs), averaged */
    i = -1;
    for (int k = 0; find_nth(0xB0, 20, k) >= 0; k++)
        i = find_nth(0xB0, 20, k);
    CHECK(i >= 0 && out[i + 2] == 127, "fader on channel 3 reads 4095 between pad scans (CC %d)",
          i >= 0 ? out[i + 2] : -1);
    /* touch: movement: EVENT touch 1 just before the movement's first value
     * EVENT, and only once; 0 after 300 ms still */
    {
        int tv = -1, av = -1, x;
        int ti = event_nth(EV_TOUCH, 0, 0, &tv), ai = event_nth(EV_ANALOG, 0, 0, &av);
        CHECK(ti >= 0 && tv == 1 && ai > ti, "fader moves: EVENT touch 1 before its first value EVENT");
        CHECK(event_nth(EV_TOUCH, 0, 1, &x) < 0, "touched once for the whole movement");
        CHECK(event_nth(EV_TOUCH, 1, 0, &x) < 0, "the XY X axis has no touch:");
    }
    out_n = 0;
    run_ms(400);
    CHECK(last_event(EV_TOUCH, 0) == 0 && last_event(EV_ANALOG, 0) < 0, "still for 300 ms: EVENT touch 0");
    settles = 0;
    run_ms(10);
#ifdef HW_ADC_TEST
    CHECK(settles == 0 && adc_reads == 0, "hardware scan: no mux settles (%d) or ADC reads (%d) in the core",
          settles, adc_reads);

    /* A strike that only one sweep sees, between two main-loop passes: the
     * peak hold still catches it (a 1 ms software read would miss it). */
    out_n = 0;
    for (uint32_t k = 0; k < SWEEPS_PER_MS; k++) {
        mux_a[5] = k == 2 ? 2800 : 0;
        sweep();
    }
    mux_a[5] = 0;
    now_ms++;
    gm_poll();
    run_ms(2);
    i = find(0x99, 41);
    CHECK(i >= 0 && out[i + 2] == 1 + 126 * 2600 / 2800 && find(0x89, 41) > i,
          "one-sweep strike on pad 6: Note On 41 velocity %d, then off (got %d)", 1 + 126 * 2600 / 2800,
          i >= 0 ? out[i + 2] : -1);
    run_ms(25);
#else
    CHECK(settles <= 10 * 8 + 5 * 2, "pads behind shared select lines switch them 8 times a scan (%d in 10 ms)",
          settles);
#endif

    /* curves, fixed, channel from midi.channel */
    out_n = 0;
    set_param(P_PAD_CURVE, CURVE_HARD);
    set_param(P_PAD_CHANNEL, 0);
    {
        const int32_t v[] = { 1600, 1600, 1600, 1600 };
        strike(&mux_a[2], v, 4);
    }
    i = find(0x90, 38);
    int lin = 1 + 126 * 1400 / 2800;
    CHECK(i >= 0 && out[i + 2] == lin * lin / 127, "hard curve on channel 1: %d (got %d)", lin * lin / 127,
          i >= 0 ? out[i + 2] : -1);
    mux_a[2] = 0;
    run_ms(25);
    out_n = 0;
    set_param(P_PAD_CURVE, CURVE_FIXED);
    {
        const int32_t v[] = { 3000, 3000, 3000, 3000 };
        strike(&mux_a[2], v, 4);
    }
    i = find(0x90, 38);
    CHECK(i >= 0 && out[i + 2] == gm_cfg.vel_fixed, "fixed: velocity.fixed");
    mux_a[2] = 0;
    run_ms(25);
    set_param(P_PAD_CURVE, CURVE_NORMAL);
    set_param(P_PAD_CHANNEL, 10);

    /* poly aftertouch while held, 0 before the Note Off */
    set_param(P_PAD_AFTERTOUCH, 1);
    out_n = 0;
    mux_a[3] = 2000;
    run_ms(30);
    i = find(0xA9, 39);
    CHECK(i >= 0 && out[i + 2] == (2000 - 100) * 127 / 2900, "poly pressure 39 = %d (got %d)",
          (2000 - 100) * 127 / 2900, i >= 0 ? out[i + 2] : -1);
    CHECK(count(0xA0) == 1, "steady pressure: sent once (%d)", count(0xA0));
    mux_a[3] = 3000;
    run_ms(30);
    CHECK(count(0xA0) == 2, "pressure up: sent again");
    out_n = 0;
    mux_a[3] = 0;
    run_ms(1);
    i = find(0xA9, 39);
    CHECK(i >= 0 && out[i + 2] == 0 && find(0x89, 39) > i, "release: pressure 0, then Note Off");
    set_param(P_PAD_AFTERTOUCH, 0);
    run_ms(25);

    /* pad.octave: the notes + 12 each; one held across a change keeps its Note Off */
    set_param(P_PAD_OCTAVE, 1);
    out_n = 0;
    mux_a[3] = 2000;
    run_ms(30);
    CHECK(find(0x99, 39 + 12) >= 0, "pad.octave 1: pad 4 plays 51");
    set_param(P_PAD_OCTAVE, -1);
    mux_a[3] = 0;
    run_ms(1);
    CHECK(find(0x89, 39 + 12) >= 0, "changed while held: the Note Off is for 51");
    run_ms(25);
    set_param(P_PAD_OCTAVE, 4);
    gm_cfg.pad_note[3] = 100;           /* 148: off the top, one octave down: 136, ... 124 */
    out_n = 0;
    mux_a[3] = 2000;
    run_ms(30);
    CHECK(find(0x99, 124) >= 0, "past 127: the highest octave in range (124)");
    mux_a[3] = 0;
    run_ms(25);
    gm_cfg.pad_note[3] = 39;
    set_param(P_PAD_OCTAVE, 0);

    /* noise: a soft reading (under twice the threshold) that is gone again
     * within PAD_MIN_MS (3 ms) is no hit; one that stays 3 ms is, and so is a
     * hard short one (the short hit below) */
    out_n = 0;
    for (int len = 1; len <= 2; len++) {
        mux_a[4] = 350;
        run_ms((uint32_t)len);
        mux_a[4] = 0;
        run_ms(25);
    }
    CHECK(count(0x90) == 0 && count(0x80) == 0, "spikes of 1 and 2 ms: no note (%d)", count(0x90));
    mux_a[4] = 350;
    run_ms(3);
    mux_a[4] = 0;
    run_ms(25);
    CHECK(count(0x90) == 1 && count(0x80) == 1, "3 ms above release: a hit (%d on, %d off)", count(0x90),
          count(0x80));

    /* thresholds are live: a higher one ignores a soft hit */
    set_param(P_PAD_THRESHOLD, 1000);
    out_n = 0;
    mux_a[4] = 800;
    run_ms(10);
    CHECK(count(0x90) == 0, "below the new threshold: nothing");
    mux_a[4] = 0;
    set_param(P_PAD_THRESHOLD, 200);
    run_ms(25);

    /* preset switch: Note Off for a held pad, which stays silent until let
     * go; the preset's note and curve */
    out_n = 0;
    mux_a[0] = 2500;
    run_ms(5);
    CHECK(find(0x99, 36) >= 0, "pad 1 held");
    presets_select(2);
    run_ms(50);
    CHECK(find(0x89, 36) >= 0 && count(0x90) == 1, "preset switch: Note Off, no new note while held");
    CHECK(gm_cfg.pad_note[0] == 70 && gm_cfg.pad_curve == CURVE_HARD && gm_cfg.pad_channel == 0,
          "preset 2: note 70, hard, midi.channel");
    mux_a[0] = 0;
    run_ms(25);
    out_n = 0;
    mux_a[0] = 2500;
    run_ms(4);
    CHECK(find(0x90, 70) >= 0, "next hit plays the preset's note 70 on channel 1");
    mux_a[0] = 0;
    run_ms(25);

    /* The XY pad: X on CC 16, Y on CC 17, over raw 592..3408. Untouched it
     * sends nothing; a touch jumps straight to where it is. */
    out_n = 0;
    run_ms(40);
    CHECK(find(0xB0, 16) < 0 && find(0xB0, 17) < 0, "XY untouched: nothing");
    CHECK(xy_reads > 0 && xy_bad == 0 && xy_short == 0, "XY: read with the right drive (%d reads, %d bad, %d shorts)",
          xy_reads, xy_bad, xy_short);
    xy_touch = 1;
    xy_x = 2000;                        /* (2000 - 592) * 4095 / 2816 = 2047: CC 63 */
    xy_y = 3408;
    run_ms(40);
    {
        int a = find(0xB0, 16), b = find(0xB0, 17);
        CHECK(a >= 0 && out[a + 2] == 63 && find_nth(0xB0, 16, 1) < 0, "touch: X = CC 16 63 at once, one message (%d)",
              a >= 0 ? out[a + 2] : -1);
        CHECK(b >= 0 && out[b + 2] == 127 && find_nth(0xB0, 17, 1) < 0, "touch: Y = CC 17 127 (%d)",
              b >= 0 ? out[b + 2] : -1);
    }
    out_n = 0;
    xy_x = 592;
    run_ms(200);                        /* software reads: each input every 6 ms, averaged */
    {
        int a = -1;
        for (int k = 0; find_nth(0xB0, 16, k) >= 0; k++)
            a = find_nth(0xB0, 16, k);
        CHECK(a >= 0 && out[a + 2] == 0 && find(0xB0, 17) < 0, "slide left: X to 0, Y stays (%d)",
              a >= 0 ? out[a + 2] : -1);
    }
    out_n = 0;
    xy_touch = 0;
    run_ms(40);
    CHECK(find(0xB0, 16) < 0 && find(0xB0, 17) < 0, "let go: nothing (it holds)");
    out_n = 0;
    xy_touch = 1;
    xy_x = 700;                         /* a finger landing: the first readings are off */
    xy_y = 1000;
    run_ms(2);
    xy_x = 3408;
    xy_y = 592;
    run_ms(40);
    {
        int a = find(0xB0, 16), b = find(0xB0, 17);
        CHECK(a >= 0 && out[a + 2] == 127 && find_nth(0xB0, 16, 1) < 0, "touch elsewhere: X jumps to 127, no glide (%d)",
              a >= 0 ? out[a + 2] : -1);
        CHECK(b >= 0 && out[b + 2] == 0 && find_nth(0xB0, 17, 1) < 0, "and Y to 0 (%d)", b >= 0 ? out[b + 2] : -1);
    }
    xy_touch = 0;
    xy_x = 4095;                        /* out of the window on one axis: no touch */
    run_ms(10);
    out_n = 0;
    xy_touch = 1;
    run_ms(40);
    CHECK(find(0xB0, 16) < 0 && find(0xB0, 17) < 0, "one axis outside the touch window: no touch");
    xy_touch = 0;
    CHECK(xy_bad == 0 && xy_short == 0, "XY drive right throughout (%d bad, %d shorts)", xy_bad, xy_short);

    /* Pad layer (modes: layer:): pad 1's own hit plays its note and lights
     * its LED (PB15); with the grid layer on, pads 1 and 2 are their twins
     * (pad components 17, 18): EVENTs, no notes, and pad 1's twin LED. */
    presets_select(1);
    run_ms(50);
    out_n = 0;
    mux_a[0] = 2500;
    run_ms(20);
    CHECK(find(0x99, 36) >= 0 && pad_event(0) < 0 && pad_event(17) < 0, "pad 1: note 36, no EVENT");
    CHECK(level[PAD1_LED] == 1, "pad 1 held: its LED lit");
    mux_a[0] = 0;
    run_ms(120);
    CHECK(find(0x89, 36) >= 0 && level[PAD1_LED] == 0, "pad 1 let go: Note Off, LED off");
    grid_tap();
    out_n = 0;
    mux_a[0] = 2500;
    run_ms(20);
    CHECK(count(0x90) == 0 && pad_event(17) == 2500 * 4 && pad_event(0) < 0,
          "grid on: pad 1 is its twin: EVENT 17 = peak x 4 (%d), no note", pad_event(17));
    CHECK(level[PAD1_LED] == 1, "twin held: the twin LED lit");
    mux_a[0] = 0;
    run_ms(120);
    CHECK(pad_event(17) == 0 && count(0x80) == 0 && level[PAD1_LED] == 0, "twin let go: EVENT 0, no Note Off, LED off");
    out_n = 0;
    mux_a[1] = 2000;
    run_ms(10);
    CHECK(count(0x90) == 0 && pad_event(18) == 2000 * 4, "pad 2's twin (no LED): EVENT 18");
    mux_a[1] = 0;
    mux_a[2] = 2000;
    run_ms(10);
    CHECK(find(0x99, 38) >= 0 && pad_event(2) < 0, "pad 3 has no twin: still its note");
    mux_a[2] = 0;
    run_ms(30);
    /* the twin LED is the host's (FB_LED); the pad's own LED keeps its state */
    {
        uint8_t fb[3] = { 1, 0, 127 };              /* LED 1 = pad_1_led_grid, on */
        host_sysex(GSX_FB_LED, fb, 3);
        run_ms(30);
        CHECK(level[PAD1_LED] == 1, "the host lights the twin LED");
        grid_tap();
        CHECK(level[PAD1_LED] == 0, "grid off: the pad's own LED (off)");
        grid_tap();
        CHECK(level[PAD1_LED] == 1, "grid on again: the host's state");
        grid_tap();
    }
    /* a hit held across the layer change stays the base pad's: its Note Off */
    out_n = 0;
    mux_a[0] = 2500;
    run_ms(10);
    grid_tap();
    mux_a[0] = 0;
    run_ms(10);
    CHECK(find(0x99, 36) >= 0 && find(0x89, 36) >= 0 && pad_event(17) < 0, "held into the layer: still the pad's");
    run_ms(30);
    out_n = 0;
    mux_a[0] = 2500;
    run_ms(10);
    grid_tap();
    mux_a[0] = 0;
    run_ms(30);
    CHECK(count(0x90) == 0 && count(0x80) == 0 && pad_event(17) == 0, "held out of the layer: still the twin's");

    /* GET_COMPONENT: pads (kind 7) are the pads, then the twins; analog touch */
    {
        const uint8_t *r;
        int rn;
        CHECK(get_component(GSX_KIND_PAD, 17, &r, &rn) == GSX_OK, "GET_COMPONENT pad 17");
        CHECK(gsx_get_u(r + 3, 2) == 19, "19 pads (17 + 2 twins): %u", (unsigned)gsx_get_u(r + 3, 2));
        r += 5;
        CHECK(lstr_is(&r, "pad_1_grid") && lstr_is(&r, "Pad 1 grid") && lstr_is(&r, "clip_1_1"), "twin id, name, role");
        CHECK(gsx_get_u(r, 2) == 1 && r[2] == 1, "its LED: the twin LED, 1; layer 1 (mode 0)");
        get_component(GSX_KIND_PAD, 1, &r, &rn);
        CHECK(r[rn - 3] == 0x7F && r[rn - 2] == 0x7F && r[rn - 1] == 0, "pad 2: no LED (0x3FFF), layer 0");
        get_component(GSX_KIND_PAD, 0, &r, &rn);
        CHECK(gsx_get_u(r + rn - 3, 2) == 0, "pad 1: LED 0");
        CHECK(get_component(GSX_KIND_PAD, 19, &r, &rn) == GSX_BAD_PARAM, "pad 19: BAD_PARAM");
        CHECK(get_component(GSX_KIND_METER, 0, &r, &rn) == GSX_BAD_PARAM, "no meters: BAD_PARAM");
        get_component(GSX_KIND_ANALOG, 0, &r, &rn);
        CHECK(r[rn - 6] == 0 && r[rn - 5] == 2, "fader: takeover none, touch 2 (movement)");
        get_component(GSX_KIND_ANALOG, 1, &r, &rn);
        CHECK(r[rn - 5] == 0, "XY X: touch none");
        CHECK(gsx_get_u(r + rn - 4, 2) == 592 && gsx_get_u(r + rn - 2, 2) == 3408, "XY X: adc_min 592, adc_max 3408");
    }

    /* the schema has the Pads group and its params */
    {
        const param_def_t *p = params_find(P_PAD_NOTE);
        CHECK(p && p->count == 17 && p->type == PT_NOTE && (p->flags & PF_PRESET) && p->defs[16] == 60,
              "pad.note: 17, note type, per preset");
        p = params_find(P_PAD_THRESHOLD);
        CHECK(p && !(p->flags & PF_PRESET) && p->def == 200, "pad.threshold: global, default 200");
        CHECK(app_params.group_count == 9 && !strcmp(app_params.groups[7], "Pads") &&
              !strcmp(app_params.groups[8], "Layers"), "groups Pads, Layers (a pad layer)");
        p = params_find(P_LAYER_ENABLED);
        CHECK(p && p->count == 1 && !(p->flags & PF_PRESET) && p->defs[0] == 1 && !strcmp(p->index_labels[0], "grid"),
              "layer.enabled: one per mode, global, labelled by id");
        CHECK(!params_find(P_LAYER_BTN_ACTION) && !params_find(P_LAYER_ENC_CC), "no button or encoder twins: none");
    }

    /* raw readings (sysex.md 4.8): the pads' and analog inputs' readings, an XY pad untouched = none */
    {
        uint8_t k = GSX_MON_PADS | GSX_MON_ANALOG | GSX_MON_ENCODERS;
        out_n = 0;
        host_sysex(GSX_CMD_MONITOR, &k, 1);
        int st = -1, kinds = -1;
        for (int i = 0; i + 1 + GSX_OFF_PAYLOAD < out_n; i++)
            if (out[i] == 0xF0 && out[i + 1 + GSX_OFF_CMD] == (GSX_CMD_MONITOR | GSX_REPLY)) {
                st = out[i + 1 + GSX_OFF_PAYLOAD];
                kinds = out[i + 2 + GSX_OFF_PAYLOAD];
            }
        CHECK(st == GSX_OK && kinds == (GSX_MON_PADS | GSX_MON_ANALOG), "MONITOR: pads and analog (no encoders)");
        xy_touch = 0;
        mux_a[2] = 1234;
        fader_mux[3] = 2000;
        run_ms(60);
        int pads = 0, analog = 0;
        for (int i = 0; i + 1 + GSX_OFF_PAYLOAD < out_n; i++) {
            if (out[i] != 0xF0 || out[i + 1 + GSX_OFF_CMD] != GSX_MSG_RAW)
                continue;
            const uint8_t *p = out + i + 1 + GSX_OFF_PAYLOAD;
            if (p[0] == EV_PAD && gsx_get_u(p + 1, 2) == 0 && p[3] == 17) {
                CHECK(gsx_get_u(p + 4 + 2 * 2, 2) == 1234 && gsx_get_u(p + 4, 2) == 0, "RAW pads: pad 3 reads 1234");
                pads = 1;
            } else if (p[0] == EV_ANALOG && p[3] == 3) {
                CHECK(gsx_get_u(p + 4, 2) > 1500 && gsx_get_u(p + 6, 2) == 0x3FFF && gsx_get_u(p + 8, 2) == 0x3FFF,
                      "RAW analog: the fader's reading (%u); XY not touched: no reading",
                      (unsigned)gsx_get_u(p + 4, 2));
                analog = 1;
            }
        }
        CHECK(pads && analog, "RAW: pads and analog");
        mux_a[2] = 0;
        k = 0;
        host_sysex(GSX_CMD_MONITOR, &k, 1);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
