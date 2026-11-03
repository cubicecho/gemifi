/*
 * test_pads.c: host test of the velocity pads, with the device
 * core/test/pads-test.yaml: 16 pads on two muxes that share their select
 * lines with a fader mux, and one inverted pad on an ADC pin. Simulated
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
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
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

/* The value of the last pad EVENT (type 7) for pad i, or -1. */
static int pad_event(int i)
{
    int v = -1;
    for (int k = 0; k + GSX_OVERHEAD < out_n; k++) {
        if (out[k] != 0xF0 || out[k + 1 + GSX_OFF_CMD] != GSX_MSG_EVENT)
            continue;
        const uint8_t *p = out + k + 1 + GSX_OFF_PAYLOAD;
        if (p[0] == EV_PAD && (int)gsx_get_u(p + 1, 2) == i)
            v = (int)gsx_get_u(p + 3, 2);
    }
    return v;
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
    CHECK(pad_event(0) == 2500 * 4, "EVENT pad 0 = peak x 4 (%d)", pad_event(0));
    run_ms(50);
    CHECK(count(0x80) == 0 && count(0x90) == 1, "held above release: one note, no off");
    mux_a[0] = 150;
    run_ms(5);
    CHECK(count(0x80) == 0, "between release and threshold: still held");
    mux_a[0] = 50;
    run_ms(1);
    CHECK(find(0x89, 36) >= 0, "below release: Note Off");
    CHECK(pad_event(0) == 0, "EVENT pad 0 = 0 on release");

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
    run_ms(60);                         /* one analog read per 2 ms, averaged */
    i = -1;
    for (int k = 0; find_nth(0xB0, 20, k) >= 0; k++)
        i = find_nth(0xB0, 20, k);
    CHECK(i >= 0 && out[i + 2] == 127, "fader on channel 3 reads 4095 between pad scans (CC %d)",
          i >= 0 ? out[i + 2] : -1);
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

    /* the schema has the Pads group and its params */
    {
        const param_def_t *p = params_find(P_PAD_NOTE);
        CHECK(p && p->count == 17 && p->type == PT_NOTE && (p->flags & PF_PRESET) && p->defs[16] == 60,
              "pad.note: 17, note type, per preset");
        p = params_find(P_PAD_THRESHOLD);
        CHECK(p && !(p->flags & PF_PRESET) && p->def == 200, "pad.threshold: global, default 200");
        CHECK(app_params.group_count == 8 && !strcmp(app_params.groups[7], "Pads"), "group Pads");
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
