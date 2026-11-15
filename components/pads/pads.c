/*
 * pads.c: the velocity pads (pads:, pad_settings:), a control component
 * (README.md here, which describes a hit): reading them and playing their
 * hits. The main loop (core/src/controller.c gm_poll) calls pads_scan once
 * per ms, which reads every pad (with the hardware analog scan: its
 * strongest reading since the last pass, vpin_analog_peak). Shares its
 * state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_vpin.h"
#include "app_proto.h"

#if GM_N_PADS > 0
#define PAD_AT_MS 10u               /* poly aftertouch: at most once per 10 ms per pad */
/* A soft hit (peak under twice pad.threshold) that falls below pad.release
 * within this is noise, not a hit (spikes while a pad board's LEDs change). */
#define PAD_MIN_MS 3u
uint8_t pad_state[NP];
static uint8_t pad_note[NP], pad_ch[NP], pad_at[NP];
static uint16_t pad_peak[NP];
static uint32_t pad_t[NP];          /* ms: strike start, last aftertouch, or release */
uint32_t pad_lit[NP];
static uint8_t pad_order[NP];       /* read order: same mux channel together */
/* Pad twins (components/modes/README.md "Button layers"): a hit goes to the pad's live
 * twin, if any: an EVENT with the twin's index, no note. Its release goes
 * to whichever took the hit. */
#if GM_PAD_TWINS
uint8_t pad_live[NP];
uint8_t pad_owner[NP];
#endif

/* Mux pads by channel, so pads behind shared select lines switch them once. */
void pads_init(void)
{
    for (uint32_t i = 0; i < NP; i++) {
        uint32_t k = i;
        uint32_t ch = gm_pads[i].pin.hub ? gm_pads[i].pin.num : 0;
        while (k > 0) {
            const gm_pin_t *p = &gm_pads[pad_order[k - 1]].pin;
            if ((p->hub ? p->num : 0u) <= ch)
                break;
            pad_order[k] = pad_order[k - 1];
            k--;
        }
        pad_order[k] = (uint8_t)i;
        pad_state[i] = PAD_BLOCKED;
    }
}

static uint32_t pad_velocity(uint32_t peak)
{
    if (gm_cfg.pad_curve == CURVE_FIXED)
        return (uint32_t)gm_cfg.vel_fixed;
    int32_t lo = gm_cfg.pad_threshold, hi = gm_cfg.pad_full;
    if (hi <= lo)
        hi = lo + 1;
    int32_t v = 1 + 126 * ((int32_t)peak - lo) / (hi - lo);
    v = v < 1 ? 1 : v > 127 ? 127 : v;
    if (gm_cfg.pad_curve == CURVE_SOFT)       /* as the keys' curves */
        v = 127 - (127 - v) * (127 - v) / 127;
    else if (gm_cfg.pad_curve == CURVE_HARD)
        v = v * v / 127;
    return (uint32_t)(v < 1 ? 1 : v);
}

/* A pad's own hit plays its note (no EVENT); a twin's sends an EVENT. */
static void pad_hit(uint32_t i, uint32_t now)
{
    pad_state[i] = PAD_ON;
    pad_t[i] = now;
    pad_lit[i] = now | 1u;
#if GM_PAD_TWINS
    pad_owner[i] = pad_live[i];
    if (pad_owner[i]) {
        send_event(EV_PAD, NP + pad_owner[i] - 1u, (uint32_t)pad_peak[i] << 2);
        return;
    }
#endif
    int32_t n = gm_cfg.pad_note[i] + 12 * gm_cfg.pad_octave;
    while (n > 127)
        n -= 12;                        /* off the top or bottom: the octave in range */
    while (n < 0)
        n += 12;
    uint32_t v = pad_velocity(pad_peak[i]);
    pad_at[i] = 0;
    pad_ch[i] = gm_cfg.pad_channel ? (uint8_t)((gm_cfg.pad_channel - 1) & 0x0F) : midi_chan();
    pad_note[i] = (uint8_t)n;
    midi3(0x90 | pad_ch[i], (uint32_t)n, v);
}

/* The end of a hit: Note Off (and pressure 0), or the twin's EVENT 0. */
static void pad_end(uint32_t i)
{
#if GM_PAD_TWINS
    if (pad_owner[i]) {
        send_event(EV_PAD, NP + pad_owner[i] - 1u, 0);
        return;
    }
#endif
    if (pad_at[i])
        midi3(0xA0 | pad_ch[i], pad_note[i], 0);
    midi3(0x80 | pad_ch[i], pad_note[i], 0);
}

static void pad_off(uint32_t i, uint32_t now)
{
    pad_end(i);
    pad_state[i] = PAD_IDLE;
    pad_t[i] = now;
}

/* Poly pressure from the reading while held: release..full -> 0..127. */
static void pad_pressure(uint32_t i, int32_t r, uint32_t now)
{
    if (now - pad_t[i] < PAD_AT_MS)
        return;
    int32_t lo = gm_cfg.pad_release, hi = gm_cfg.pad_full;
    if (hi <= lo)
        hi = lo + 1;
    int32_t v = (r - lo) * 127 / (hi - lo);
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    int32_t d = v - pad_at[i];
    if (d < 2 && d > -2 && !(v == 0 && pad_at[i]))
        return;
    pad_at[i] = (uint8_t)v;
    pad_t[i] = now;
    midi3(0xA0 | pad_ch[i], pad_note[i], (uint32_t)v);
}

#if GM_MONITOR
uint16_t pad_raw[NP];
#define PAD_RAW(i, r) (pad_raw[i] = (uint16_t)(r))
#else
#define PAD_RAW(i, r) ((void)0)
#endif

void pads_scan(uint32_t now)
{
    int32_t on = gm_cfg.pad_threshold, off = gm_cfg.pad_release;
    if (off >= on)
        off = on - 1;
    vpin_analog_peaks();            /* hardware analog scan: peaks since the last pass */
    for (uint32_t k = 0; k < NP; k++) {
        uint32_t i = pad_order[k];
        int32_t r = vpin_analog_peak(gm_pads[i].pin);
        if (r < 0)
            continue;
        PAD_RAW(i, r);
        switch (pad_state[i]) {
        case PAD_IDLE:
            if (r < on || now - pad_t[i] < (uint32_t)gm_cfg.pad_retrigger)
                break;
            pad_state[i] = PAD_PEAK;
            pad_t[i] = now;
            pad_peak[i] = (uint16_t)r;
            break;
        case PAD_PEAK:
            if (r < off && now - pad_t[i] < PAD_MIN_MS && pad_peak[i] < 2 * on) {
                pad_state[i] = PAD_IDLE;    /* a soft spike, gone again at once */
                break;
            }
            if (r > pad_peak[i])
                pad_peak[i] = (uint16_t)r;
            if (r >= off && now - pad_t[i] < (uint32_t)gm_cfg.pad_window)
                break;
            pad_hit(i, now);            /* window over, or already let go */
            /* fall through */
        case PAD_ON:
            if (r < off)
                pad_off(i, now);
            else if (gm_cfg.pad_aftertouch && !PAD_OWNER(i))
                pad_pressure(i, r, now);
            break;
        default:                        /* PAD_BLOCKED */
            if (r < off) {
                pad_state[i] = PAD_IDLE;
                pad_t[i] = now;
            }
            break;
        }
    }
}

/* Note Off for every sounding pad (a twin's: EVENT 0); held pads stay
 * silent until let go. */
void pads_off(void)
{
    for (uint32_t i = 0; i < NP; i++) {
        if (pad_state[i] == PAD_ON || pad_state[i] == PAD_PEAK) {
            if (pad_state[i] == PAD_ON)
                pad_end(i);
            pad_state[i] = PAD_BLOCKED;
        }
    }
}

/* At boot-log time: say which pads never read below pad.release. */
void pads_report(void)
{
    for (uint32_t i = 0; i < NP; i++)
        if (pad_state[i] == PAD_BLOCKED)
            app_log(LOG_WARN, gm_pads[i].name, ": reads above pad.release at rest (inverted? held?)");
}
#endif
