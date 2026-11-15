/*
 * controller.c: the generic MIDI controller runtime: MIDI out and the MIDI
 * messages the controls send, the scan, the preset release, gm_init and
 * gm_poll. The controls themselves are components (components/buttons,
 * leds, analog, encoders, pads, modes, key_matrix), sharing state with this
 * file through gm_controls.h. See gm_controller.h and core/README.md.
 *
 * Split between contexts:
 *   gm_scan_tick() (timer interrupt): the key matrix (matrix_scan),
 *     encoders not on I2C hubs (encoder_step), shift-register hub
 *     refresh/flush. Key changes go to the main loop through a small queue.
 *   gm_scan_frames() (the platform's DMA interrupt, same level), with a
 *     hardware scan instead: the matrices and their encoders from finished
 *     frames; the scan tick then leaves those alone.
 *   gm_poll() (main loop): everything that sends MIDI, buttons, analog
 *     inputs and pads, ramps, LEDs, I2C and TM1628 hubs, the protocol.
 * Host feedback (gm_fb_msg in feedback.c, from midi_in.c) also runs in the
 * main loop: the platform feeds received MIDI from its main loop.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controller.h"
#include "gm_controls.h"
#include "gm_device.h"
#include "gm_device_config.h"
#if GM_N_SCREENS > 0
#include "gm_screen.h"                  /* display: (components/display) */
#endif
#include "gm_settings.h"
#include "gm_vpin.h"
#include "gm_hal.h"
#include "app_proto.h"
#include "params.h"
#include "gm_presets.h"
#include "gm_feedback.h"
#include "gm_mcu.h"
#include "gm_display.h"
#include "gm_din.h"
#include "gm_safety.h"

#define I2C_MS          1u
#define BOOT_LOG_MS     1500u

static uint32_t fw_ver;
static uint8_t has_sr_hubs;         /* any VPIN_SCAN_IO hub (74HC595/165): refresh in the tick */
static uint8_t has_matrices;        /* any switch-matrix hub: scan in the tick */
static uint8_t has_i2c_hubs;        /* any VPIN_BUS_IO hub (I2C): every I2C_MS */
static uint8_t has_main_hubs;       /* any VPIN_MAIN hub (TM1628, MAUDIO_PADS): vpin_poll every pass */

/* ---- MIDI out ---------------------------------------------------------- */

/* write_to_flash.last_preset: when the last local input was (gm_idle_ms;
 * the keys held: keys_down). One store per message or EVENT; nothing
 * without it. */
#if GM_WRITE_LAST_PRESET
static uint32_t in_t;
#define INPUT_NOW() (in_t = hal_millis())
#else
#define INPUT_NOW() ((void)0)
#endif

uint8_t midi_chan(void)
{
    return (uint8_t)((gm_cfg.channel - 1) & 0x0F);
}

void midi3(uint8_t status, uint32_t a, uint32_t b)
{
    uint8_t m[3] = { status, (uint8_t)(a & 0x7F), (uint8_t)(b & 0x7F) };
    INPUT_NOW();
    hal_midi_send(m, 3);
    gm_din_local(m, 3);
    gm_display_midi(m[0], m[1], m[2]);
}

#if GM_N_BUTTONS > 0 || GM_N_ANALOGS > 0
void midi2(uint8_t status, uint32_t a)
{
    uint8_t m[2] = { status, (uint8_t)(a & 0x7F) };
    hal_midi_send(m, 2);
    gm_din_local(m, 2);
    gm_display_midi(m[0], m[1], 0);
}
#endif

void send_event(uint8_t type, uint32_t index, uint32_t value)
{
    INPUT_NOW();
    if (gm_cfg.events)
        app_event(type, index, value);
}

/* ---- MIDI messages (core/README.md "MIDI messages") -------------------- */

#if GM_CTL
/* A control's channel: its channel:, else midi.channel. */
uint8_t ctl_chan(const gm_ctl_t *t)
{
    return t->channel ? (uint8_t)((t->channel - 1u) & 0x0Fu) : midi_chan();
}

int32_t ctl_hi(const gm_ctl_t *t, int32_t top)
{
    return t->max == GM_CTL_TOP ? top : (int32_t)t->max;
}

/* v of 0..top as the control's min..max, rounded (min > max: inverted). */
uint32_t ctl_out(const gm_ctl_t *t, int32_t v, int32_t top)
{
    int32_t lo = t->min, d = ctl_hi(t, top) - lo;
    return (uint32_t)(lo + (v * d + (d < 0 ? -top : top) / 2) / top);
}
#endif

#if GM_MSG_KINDS
/* A kind whose values are 14-bit (a SysEx string with msb / lsb in it). */
int kind_wide(uint32_t kind, uint32_t num)
{
#if GM_N_SYSEX
    if (kind == K_SYSEX)
        return num < GM_N_SYSEX && (gm_sysex[num].flags & GM_SX_WIDE);
#endif
    (void)num;
    return kind == K_CC14 || kind == K_NRPN14 || kind == K_RPN14;
}
#endif

#if GM_N_SYSEX || GM_MSG_MMC
void sysex_out(const uint8_t *m, uint32_t n)
{
    INPUT_NOW();
    hal_midi_send(m, n);
    gm_din_local(m, n);
}
#endif

#if GM_N_SYSEX
/* SysEx string k with value v and channel ch (0..15) filled in. */
void sysex_send(uint32_t k, uint32_t v, uint8_t ch)
{
    const gm_sysex_t *s = &gm_sysex[k];
    uint8_t m[GM_SYSEX_MAX];
    uint32_t n = 0, sum = 0;
    for (uint32_t i = 0; i < s->len && n < GM_SYSEX_MAX; i++) {
        uint32_t b = s->data[i];
        switch (b) {
        case GM_SX_VALUE:
        case GM_SX_LSB:
            b = v & 0x7Fu;
            break;
        case GM_SX_MSB:
            b = (v >> 7) & 0x7Fu;
            break;
        case GM_SX_CHANNEL:
            b = ch;
            break;
        case GM_SX_SUM_START:
            sum = 0;
            continue;
        case GM_SX_SUM:
            b = (128u - (sum & 127u)) & 127u;      /* Roland */
            break;
        default:
            break;
        }
        sum += b;
        m[n++] = (uint8_t)b;
    }
    sysex_out(m, n);
}
#endif

#if GM_MSG_KINDS
/* Value v (7- or 14-bit, as kind wants) as kind on channel ch: num is the
 * CC, the (N)RPN number or the SysEx index. */
void send_value(uint32_t kind, uint8_t ch, uint32_t num, uint32_t v)
{
    uint8_t st = (uint8_t)(0xB0u | ch);
    switch (kind) {
#if GM_MSG_CC14
    case K_CC14:                    /* MSB, then the LSB on cc + 32 */
        midi3(st, num, v >> 7);
        midi3(st, num + 32u, v);
        return;
#endif
#if GM_MSG_NRPN
    case K_NRPN:
    case K_NRPN14:
    case K_RPN:
    case K_RPN14: {
        uint32_t sel = kind >= K_RPN ? 101u : 99u;     /* the number's MSB; LSB one below */
        midi3(st, sel, num >> 7);
        midi3(st, sel - 1u, num);
        if (kind == K_NRPN14 || kind == K_RPN14) {
            midi3(st, 6, v >> 7);   /* data entry MSB, LSB */
            midi3(st, 38, v);
        } else {
            midi3(st, 6, v);
        }
#if GM_NULL_RPN
        midi3(st, 101, 127);        /* the null RPN: later data entry goes nowhere */
        midi3(st, 100, 127);
#endif
        return;
    }
#endif
#if GM_N_SYSEX
    case K_SYSEX:
        if (num < GM_N_SYSEX)
            sysex_send(num, v, ch);
        return;
#endif
    default:
        midi3(st, num, v);
        return;
    }
}
#endif

/* ---- the scan ---------------------------------------------------------- */

volatile uint32_t scan_ticks;
volatile uint32_t gm_scan_period_us;
volatile uint32_t gm_scan_part_max[3];
uint32_t gm_poll_part_max[GM_POLL_PARTS];

__attribute__((weak)) uint32_t hal_cycles(void)
{
    return 0;
}

static void part_done(uint32_t i, uint32_t *t)
{
    uint32_t now = hal_cycles(), d = now - *t;
    if (d > gm_scan_part_max[i])
        gm_scan_part_max[i] = d;
    *t = now;
}

static void poll_part_done(uint32_t i, uint32_t *t)
{
    uint32_t now = hal_cycles(), d = now - *t;
    if (d > gm_poll_part_max[i])
        gm_poll_part_max[i] = d;
    *t = now;
}

/* ---- hardware scan (gm_controller.h gm_scan_plan_t) -------------------- */

uint8_t hw_on;
#if GM_HW_SCAN
uint32_t hw_frame_q4;
/* The most plan rows this device can have (the builder's GM_HW_SCAN_ROWS):
 * platform rings are sized for it. */
#ifdef GM_HW_SCAN_ROWS
#define HW_ROWS GM_HW_SCAN_ROWS
#else
#define HW_ROWS GM_SCAN_MAX_ROWS
#endif
static gm_scan_plan_t plan;
static uint32_t hw_prev[HW_ROWS / 2u];   /* the last frame that differed */
static uint32_t hw_mask;            /* column bits of both halfwords of a frame word */
#if GM_N_ENCODERS > 0
static uint8_t enc_fed[NE];         /* both pins on switch matrices: stepped per frame */
#endif
typedef uint32_t __attribute__((may_alias)) u32_alias;

static int on_swmatrix(gm_pin_t p)
{
    return (vpin_pin_caps(p) & VPIN_MATRIX) != 0;
}
#endif

int gm_scan_hw(void)
{
    return hw_on;
}

const gm_scan_plan_t *gm_scan_plan(void)
{
#if GM_HW_SCAN
    plan.sel_reg = 0;
    plan.in_reg = 0;
    plan.n_rows = 0;
    plan.in_mask = 0;
    plan.row_ns = GM_HW_SCAN_ROW_NS;
    plan.settle_ns = GM_HW_SCAN_SETTLE_NS;
    plan.precharge = GM_HW_SCAN_PRECHARGE;
#if GM_HAS_MATRIX
    int keys = matrix_plan(&plan, HW_ROWS);    /* key rows: plan slots 0..n_rows-1 */
    if (keys == 0)
        return 0;
    if (keys < 0)
        goto mixed;
#endif
    if (!vpin_scan_plan(&plan.sel_reg, plan.sel_word, &plan.n_rows, HW_ROWS,
                        &plan.in_reg, &plan.in_mask))
        goto mixed;
    if (!plan.n_rows || !plan.sel_reg || !plan.in_reg || (plan.in_mask >> 16)) {
        app_log(LOG_WARN, "hardware scan: nothing to scan, or columns above bit 15; ",
                "scanning in software");
        return 0;
    }
    if (plan.n_rows & 1u) {
        if (plan.n_rows >= HW_ROWS)
            goto mixed;
        plan.sel_word[plan.n_rows] = plan.sel_word[plan.n_rows - 1u];
        plan.n_rows++;
    }
    return &plan;
mixed:
    app_log(LOG_WARN, "hardware scan: rows or columns of the matrices are on different ",
            "registers, or too many rows; scanning in software");
#endif
    return 0;
}

void gm_scan_hw_started(uint32_t frame_q4)
{
#if GM_HW_SCAN
    hw_frame_q4 = frame_q4;
    hw_mask = (plan.in_mask & 0xFFFFu) * 0x10001u;
    for (uint32_t i = 0; i < HW_ROWS / 2u; i++)
        hw_prev[i] = 0xFFFFFFFFu;   /* the first frame always counts */
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < NE_HW; i++)
        enc_fed[i] = (uint8_t)(on_swmatrix(gm_device.encoders[i].a) &&
                               on_swmatrix(gm_device.encoders[i].b));
#endif
    vpin_scan_hw(1);
    hw_on = 1;
#else
    (void)frame_q4;
#endif
}

void gm_scan_frames(const uint16_t *raw, uint32_t n, uint32_t lost)
{
#if GM_HW_SCAN
    uint32_t words = plan.n_rows / 2u, key_rows = 0;
#if GM_HAS_MATRIX
    key_rows = gm_device.matrix ? gm_device.matrix->n_rows : 0u;
#endif
    scan_ticks += lost;
    for (uint32_t f = 0; f < n; f++, raw += plan.n_rows) {
        scan_ticks++;
        /* Word k holds rows 2k and 2k+1 (a key pair on a row-pair keybed);
         * only the column bits count (the rest of the port may be anything).
         * Nothing moved since the last frame (nearly always): nothing to do. */
        const u32_alias *w = (const u32_alias *)(const void *)raw;
        uint32_t chg = 0;
        for (uint32_t k = 0; k < words; k++) {
            uint32_t x = w[k] & hw_mask;
            if (x != hw_prev[k]) {
                chg |= 1u << k;
                hw_prev[k] = x;
            }
        }
#if GM_HAS_MATRIX
        chg |= matrix_retry(key_rows);  /* a key change didn't fit: every key row again */
#endif
        if (!chg)
            continue;
#if GM_HAS_MATRIX
        matrix_frame(raw, chg, key_rows);
#endif
        if (!(chg >> (key_rows / 2u)))
            continue;                   /* only key rows changed */
        vpin_scan_feed(raw);
#if GM_N_ENCODERS > 0
        for (uint32_t i = 0; i < NE_HW; i++)
            if (enc_fed[i])
                encoder_step(i);
#endif
    }
#else
    (void)raw;
    (void)n;
    (void)lost;
#endif
}

void gm_scan_tick(void)
{
    if (!hw_on)
        scan_ticks++;               /* hardware scan: frames count the time */
    uint32_t t = hal_cycles();
    if (has_sr_hubs)
        vpin_refresh();
#if GM_HAS_MATRIX
    if (gm_device.matrix) {
        if (!hw_on)
            matrix_scan();
        kt0_age();                  /* the frames' keys too */
    }
#endif
    part_done(0, &t);
    /* Switch matrices (panel buttons, encoders) every other tick: plenty for
     * both, and it halves their share of the scan. */
    if (has_matrices && !hw_on && (scan_ticks & 1u))
        vpin_scan_matrices();       /* before the encoders read them */
    part_done(1, &t);
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < NE_HW; i++) {
#if GM_HW_SCAN
        if (hw_on && enc_fed[i])
            continue;
#endif
        if (!enc_i2c[i])
            encoder_step(i);
    }
#endif
    part_done(2, &t);
    if (has_sr_hubs)
        vpin_flush();
}

/* ---- controls the device doesn't have ---------------------------------- */

/* The protocol and host feedback call these whatever the device has; without
 * the control (its component isn't built) they do nothing. */
#if GM_N_MODES == 0
void gm_layers_report(void)
{
}
#endif
#if GM_N_ANALOGS == 0
void gm_analog_host(uint32_t i, uint32_t value)
{
    (void)i;
    (void)value;
}
#endif
#if GM_N_ENCODERS == 0
void gm_encoder_host(uint32_t i, uint32_t value)
{
    (void)i;
    (void)value;
}
#endif

/* ---- idle time (write_to_flash.last_preset) ---------------------------- */

#if GM_WRITE_LAST_PRESET
uint32_t gm_idle_ms(void)
{
#if GM_HAS_MATRIX
    if (keys_down)
        return 0;
#endif
#if GM_N_BUTTONS > 0
    if (btn_stable)
        return 0;
#endif
    return hal_millis() - in_t;
}
#endif

/* ---- preset switch: let go of everything ------------------------------- */

void gm_release_all(void)
{
#if GM_HAS_MATRIX
    keys_off();
#endif
#if GM_N_BUTTONS > 0
    buttons_off();
#endif
#if GM_N_ANALOGS > 0
    analog_pickup_off();
#endif
#if GM_N_PADS > 0
    pads_off();
#endif
}

/* ---- init -------------------------------------------------------------- */

uint32_t gm_buttons_now(void)
{
    vpin_init();
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        vpin_input(gm_device.buttons[i].pin);
    hal_delay_us(50);
    vpin_refresh();
    vpin_refresh_i2c();
    vpin_scan_matrices();
    uint32_t s = 0;
    for (uint32_t i = 0; i < gm_device.n_buttons && i < 32; i++)
        if (vpin_read(gm_device.buttons[i].pin))
            s |= 1u << i;
    return s;
}

static uint8_t safe;

void gm_init(uint32_t fw_version, int safe_mode)
{
    fw_ver = fw_version;
    safe = (uint8_t)safe_mode;
    gm_scan_period_us = gm_device.scan_us;
    uint32_t caps = vpin_caps();
    has_sr_hubs = (caps & VPIN_SCAN_IO) != 0;
    has_i2c_hubs = (caps & VPIN_BUS_IO) != 0;
    has_matrices = (caps & VPIN_MATRIX) != 0;
    has_main_hubs = (caps & VPIN_MAIN) != 0;
    vpin_init();
#if GM_N_SCREENS > 0
    gm_screen_init();
#endif
    gm_din_init();
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        vpin_input(gm_device.buttons[i].pin);
#if GM_N_MODES > 0
    modes_init();                   /* no mode on, no cycle on a step: those wait for the settings (below) */
#endif
    layers_update();                /* no mode yet: every base live, every twin hidden */
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        vpin_output(gm_device.leds[i].pin);
        if (!vpin_is_none(gm_device.leds[i].pin2))
            vpin_output(gm_device.leds[i].pin2);
        if (!vpin_is_none(gm_device.leds[i].pin3))
            vpin_output(gm_device.leds[i].pin3);
    }
#if GM_N_ENCODERS > 0
    encoders_init();
#endif
#if GM_HAS_MATRIX
    if (gm_device.matrix)
        matrix_init();
#endif
#if GM_N_BUTTONS > 0
    buttons_init();
#endif
#if GM_N_ANALOGS > 0
    analog_init();
#endif
#if GM_N_PADS > 0
    pads_init();
#endif

    leds_init();
#if GM_FEEDBACK
    feedback_init();
#endif
#if GM_MCU
    gm_mcu_init(fw_version);
#endif

    gm_settings_init();
    presets_boot(safe);
    gm_safety_init(presets_boot_state());
#if GM_N_CYCLES
    cycles_start();                 /* each cycle on its first enabled step */
#endif
    layers_update();
    app_log_limit = (uint8_t)gm_cfg.log_level;
    app_proto_init(fw_version, 1);

    /* Lamp test: every LED on briefly. */
    leds_lamp(1);
    vpin_flush();
    vpin_flush_i2c();
    vpin_send_all();
#if GM_N_SCREENS > 0
    (void)gm_screen_send(0xFFFFFFFFu);
#endif
    hal_delay_us(200000);
    leds_lamp(0);
    vpin_flush();
    vpin_flush_i2c();
    vpin_send_all();
#if GM_N_SCREENS > 0
    (void)gm_screen_send(0xFFFFFFFFu);
#endif
}

/* ---- main loop --------------------------------------------------------- */

void gm_poll(void)
{
    static uint32_t t_led, t_i2c;
#if GM_N_BUTTONS > 0
    static uint32_t t_ms, t_btn;
#endif
#if GM_N_ANALOGS > 0
    static uint32_t t_analog;
#endif
#if GM_N_PADS > 0
    static uint32_t t_pad;
#endif
    static uint8_t booted;
    uint32_t now = hal_millis();
    uint32_t tc = hal_cycles();

    /* Keys and buttons first, and their MIDI to the host at once: nothing
     * below (protocol, presets, analog, LEDs, the display, the TM1628 bus)
     * stands between a key and its USB packet. Keys still go before buttons,
     * as before. Part numbers name the part, not the order. */
#if GM_HAS_MATRIX
    keys_poll();
#endif
#if GM_N_BUTTONS > 0
    if (now - t_btn >= BTN_SAMPLE_MS) {
        t_btn = now;
        buttons_sample();           /* I2C hub inputs: as refreshed on an earlier pass */
    }
    if (now - t_ms > 50u)
        t_ms = now - 1u;            /* first call, or a long stall (flash erase) */
    while (t_ms != now) {
        t_ms++;
        ramps_ms(now);
    }
#endif
    hal_midi_poll();
    poll_part_done(1, &tc);

    app_proto_poll();
#if GM_N_BUTTONS > 0
    gm_safety_poll(now, btn_stable);   /* the combos, the status LED, diag.* (gm_safety.h) */
#else
    gm_safety_poll(now, 0);
#endif
#if GM_LAYER_PARAMS
    layers_report_poll();
#endif
    gm_din_poll(now);
    presets_poll(now);
    app_log_limit = (uint8_t)gm_cfg.log_level;
    if (has_i2c_hubs && now - t_i2c >= I2C_MS) {
        t_i2c = now;
        vpin_flush_i2c();
        vpin_refresh_i2c();
    }
    poll_part_done(0, &tc);
#if GM_N_ANALOGS > 0
    if (now - t_analog >= (gm_adc_hw() ? 1u : ANALOG_MS)) {
        t_analog = now;
        analog_poll();
    }
#endif
    poll_part_done(2, &tc);
#if GM_N_PADS > 0
    if (now - t_pad >= PAD_MS) {
        t_pad = now;
        pads_scan(now);
    }
#endif
    poll_part_done(3, &tc);
#if GM_N_ENCODERS > 0
    encoders_poll(now);
#endif
#if GM_MONITOR
    monitor_poll(now);
#endif
#if GM_HAS_MATRIX
    keys_poll();                    /* keys the scan queued since the top: not a whole pass later */
#endif
    hal_midi_poll();                /* analog, pads, encoders, DIN merged in: before the LEDs too */
    poll_part_done(4, &tc);
    int led_pass = 0;
    if (now - t_led >= LED_MS) {
        t_led = now;
        leds_update(now);           /* the buttons' and pads' levels */
#if defined(GM_DISPLAY_SHOWS) && ((GM_DISPLAY_SHOWS >> GM_SHOWS_CHANNEL) & 1)
        gm_display_channel((uint32_t)gm_cfg.channel);   /* a channel display: redrawn on a change */
#endif
        gm_display_poll(now);       /* the displays' segments: written with the LEDs */
        led_pass = 1;
    } else {
        leds_apply();               /* then the LEDs, a few per pass */
    }
    poll_part_done(5, &tc);
    /* The main-loop hubs: changed TM1628 bytes, a few per pass, and not on
     * the LED update's pass (busy), so the two never add up in one pass.
     * With fade: chips, not while a round is being shown either (leds_fade).
     * The polls (MAUDIO_PADS) run on every pass. */
    int round_open = leds_round_open();
#if GM_N_SCREENS > 0
    /* The screens (display:) send what the hubs left of the bus budget. */
    uint32_t left = vpin_poll(now, led_pass || round_open);
    if (left)
        (void)gm_screen_send(left);
#else
    if (has_main_hubs)
        vpin_poll(now, led_pass || round_open);
#endif
    poll_part_done(6, &tc);
    if (!booted && now >= BOOT_LOG_MS) {
        booted = 1;
        app_log(LOG_INFO, gm_device.name, safe ? " up (safe mode: defaults)" : " up");
        app_log_hex(LOG_INFO, "fw ", fw_ver);
#if GM_N_PADS > 0
        pads_report();
#endif
    }
#if GM_HAS_MATRIX
    keys_poll();                    /* and after the LEDs and the TM1628 bus */
#endif
    hal_midi_poll();
}
