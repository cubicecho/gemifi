/*
 * buttons.c: the buttons (buttons:), a control component (README.md here):
 * sampling and debouncing them, their actions, the CC ramps and the pitch
 * bend buttons' glide. The main loop (core/src/controller.c gm_poll) calls
 * buttons_sample and ramps_ms; a button a mode takes goes to the modes
 * (mode_button, mode_btn). Shares its state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_display.h"
#include "gm_presets.h"
#include "gm_safety.h"
#include "gm_mcu.h"
#include "gm_vpin.h"
#include "params.h"

#if GM_N_BUTTONS > 0
#define BEND_SEND_MS 2u             /* pitch bend buttons: at most one bend per 2 ms while it glides */
uint64_t btn_stable, btn_toggled;
/* Held, and the press went to this button: a layer twin and its base read
 * the same pin, but only the live one takes the press (and its release). */
uint64_t btn_held;
#if GM_LAYERS
uint64_t btn_live = ~0ull;
#define BTN_LIVE_ALL btn_live
#else
#define BTN_LIVE_ALL (~0ull)
#endif
btn_bits_t btn_hist[BTN_STABLE - 1u];
static uint64_t btn_ignore;         /* held through a preset switch: release is silent */
int16_t ramp_q8[NB];
static int16_t ramp_sent[NB];
static uint8_t btn_note[NB], btn_chan[NB];
static int32_t bend, bend_target, bend_sent = 0x7FFFFFFF;
static int32_t program;

/* A button's CC values (min:, max:; 0 and 127 by default). */
#if GM_BTN_CTL
#define BTN_OUT(i, v, top) ctl_out(&gm_button_ctl[i], (int32_t)(v), (top))
#define BTN_CC(i, num, v) midi3((uint8_t)(0xB0u | BTN_CH(i)), (num), BTN_OUT(i, v, 127))
#else
#define BTN_OUT(i, v, top) ((uint32_t)(v))
#define BTN_CC(i, num, v) cc((num), (v))
#endif

/* The Program Change of a program action, after the button's bank: if it
 * has one. */
static void program_send(uint32_t i, uint32_t pc)
{
    uint8_t ch = BTN_CH(i);
#if GM_BTN_BANK
    uint32_t bank = gm_button_bank[i];
    if (bank != GM_BANK_NONE) {
        midi3((uint8_t)(0xB0u | ch), 0, bank >> 7);
        midi3((uint8_t)(0xB0u | ch), 32, bank);
    }
#else
    (void)i;
#endif
    midi2((uint8_t)(0xC0u | ch), pc);
}

#if GM_MSG_PANIC
/* Panic: let go of every note gemifi holds (keys, buttons, pads, CCs held
 * on, pitch bend), as a preset switch does, then All Sound Off, All Notes
 * Off and Reset All Controllers on every channel. */
static void panic(void)
{
    gm_release_all();
    for (uint32_t c = 0; c < 16u; c++) {
        midi3((uint8_t)(0xB0u | c), 120, 0);
        midi3((uint8_t)(0xB0u | c), 123, 0);
        midi3((uint8_t)(0xB0u | c), 121, 0);
    }
}
#endif

/* Pressed in every scan since the last sample; *any: in any of them. A
 * button on a switch matrix is scanned many times a sample (each frame of
 * a hardware scan), others are read once. */
static btn_bits_t read_buttons(btn_bits_t *any)
{
    btn_bits_t s = 0, a = 0;
    vpin_span_take();
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
        int seen;
        if (vpin_read_span(gm_device.buttons[i].pin, &seen))
            s |= (btn_bits_t)1 << i;
        if (seen)
            a |= (btn_bits_t)1 << i;
    }
    *any = a;
    return s;
}

static int held_with_action(int32_t act, uint32_t except)
{
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++)
        if (i != except && ((btn_held >> i) & 1u) && gm_cfg.btn_action[i] == act)
            return 1;
    return 0;
}

static void bend_retarget(void)
{
    int dn = held_with_action(ACT_BEND_DOWN, 0xFFFFu);
    int up = held_with_action(ACT_BEND_UP, 0xFFFFu);
    bend_target = (dn && !up) ? -8192 : (up && !dn) ? 8191 : 0;
}

void shift(int32_t *v, int32_t step, int32_t lim, uint16_t id, int reset)
{
    int32_t n = reset ? 0 : *v + step;
    if (n < -lim)
        n = -lim;
    if (n > lim)
        n = lim;
    if (n != *v) {
        *v = n;
        app_param_changed(id, 0);
    }
    /* "OCT +1", "TR -12", "OCT 0" on the display, also at a limit */
    char t[8];
    uint32_t j = 0, a = (uint32_t)(n < 0 ? -n : n);
    const char *l = id == P_OCTAVE ? "OCT " : id == P_TRANSPOSE ? "TR " : "PAD ";
    while (*l)
        t[j++] = *l++;
    if (n)
        t[j++] = n < 0 ? '-' : '+';
    if (a >= 10u)
        t[j++] = (char)('0' + a / 10u % 10u);
    t[j++] = (char)('0' + a % 10u);
    t[j] = 0;
    gm_display_message(t);
}

static void button_changed(uint32_t i, int down)
{
    int32_t act = gm_cfg.btn_action[i];
    uint32_t num = (uint32_t)gm_cfg.btn_number[i];
    send_event(EV_BUTTON, i, (uint32_t)down);
    app_log(LOG_DEBUG, gm_device.buttons[i].name, down ? " down" : " up");
    switch (act) {
    case ACT_CC_MOMENTARY:
        BTN_CC(i, num, down ? 127 : 0);
        break;
    case ACT_CC_TOGGLE:
        if (down) {
            btn_toggled ^= 1ull << i;
            BTN_CC(i, num, ((btn_toggled >> i) & 1u) ? 127 : 0);
        }
        break;
    case ACT_NOTE:
        if (down) {
            btn_note[i] = (uint8_t)num;
            btn_chan[i] = BTN_CH(i);
            midi3(0x90 | btn_chan[i], num, (uint32_t)gm_cfg.vel_fixed);
        } else if (btn_note[i] != NOTE_NONE) {
            midi3(0x80 | btn_chan[i], btn_note[i], 0);
            btn_note[i] = NOTE_NONE;
        }
        break;
    case ACT_OCTAVE_DOWN:
    case ACT_OCTAVE_UP:
        if (down) {
            int other = held_with_action(act == ACT_OCTAVE_UP ? ACT_OCTAVE_DOWN : ACT_OCTAVE_UP, i);
            shift(&gm_cfg.octave, act == ACT_OCTAVE_UP ? 1 : -1, 4, P_OCTAVE, other);
        }
        break;
    case ACT_TRANSPOSE_DOWN:
    case ACT_TRANSPOSE_UP:
        if (down) {
            int other = held_with_action(act == ACT_TRANSPOSE_UP ? ACT_TRANSPOSE_DOWN : ACT_TRANSPOSE_UP, i);
            shift(&gm_cfg.transpose, act == ACT_TRANSPOSE_UP ? 1 : -1, 24, P_TRANSPOSE, other);
        }
        break;
    case ACT_BEND_DOWN:
    case ACT_BEND_UP:
        bend_retarget();
        break;
#if GM_N_PADS > 0
    case ACT_PAD_OCTAVE_DOWN:
    case ACT_PAD_OCTAVE_UP:
        if (down) {     /* both held: back to 0, as the octave buttons */
            int other = held_with_action(act == ACT_PAD_OCTAVE_UP ? ACT_PAD_OCTAVE_DOWN : ACT_PAD_OCTAVE_UP, i);
            shift(&gm_cfg.pad_octave, act == ACT_PAD_OCTAVE_UP ? 1 : -1, 4, P_PAD_OCTAVE, other);
        }
        break;
#endif
    case ACT_PROGRAM_NEXT:
    case ACT_PROGRAM_PREV:
        if (down) {
            program = (program + (act == ACT_PROGRAM_NEXT ? 1 : 127)) & 127;
            program_send(i, (uint32_t)program);
        }
        break;
#if GM_MSG_PROGRAM
    case ACT_PROGRAM:
        if (down) {
            program = (int32_t)(num & 127u);    /* program next / previous go on from it */
            program_send(i, num);
        }
        break;
#endif
#if GM_MSG_PANIC
    case ACT_PANIC:
        if (down)
            panic();
        break;
#endif
#if GM_MSG_MMC
    case ACT_MMC:
        if (down) {
            const uint8_t m[6] = { 0xF0, 0x7F, GM_MMC_DEVICE, 0x06, (uint8_t)(num & 0x7Fu), 0xF7 };
            sysex_out(m, 6);
        }
        break;
#endif
#if GM_N_SYSEX
    case ACT_SYSEX:
        /* press: the value max: (top by default), release: min: (0), if the
         * string has a value at all */
        if (num < GM_N_SYSEX && (down || (gm_sysex[num].flags & GM_SX_HAS_VALUE))) {
            uint32_t top = (gm_sysex[num].flags & GM_SX_WIDE) ? 16383u : 127u;
            sysex_send(num, BTN_OUT(i, down ? top : 0u, (int32_t)top), BTN_CH(i));
        }
        break;
#endif
    case ACT_PRESET_NEXT:
    case ACT_PRESET_PREV:
        if (down)
            presets_step(act == ACT_PRESET_NEXT ? 1 : -1);
        break;
    case ACT_PRESET_SELECT:
        if (down && num >= 1 && num <= GM_N_PRESETS)
            presets_select(num);
        break;
#if GM_MCU
    case ACT_MCU:
        gm_mcu_note(num, down);
        break;
#endif
    default:                        /* ACT_NONE, ACT_CC_RAMP (in ramps_ms) */
        break;
    }
}

void buttons_sample(void)
{
    /* A press counts once a sample saw it in every scan (a contact that
     * only flickered closed, or noise, doesn't press); any scan that saw it
     * closed keeps it held for BTN_STABLE more samples. */
    btn_bits_t seen;
    btn_bits_t now = read_buttons(&seen);
    btn_bits_t recent = seen;       /* seen pressed in any of the last BTN_STABLE samples */
    for (uint32_t k = 0; k < BTN_STABLE - 1u; k++)
        recent |= btn_hist[k];
    for (uint32_t k = BTN_STABLE - 1u; k > 1u; k--)
        btn_hist[k - 1u] = btn_hist[k - 2u];
    btn_hist[0] = seen;
    uint64_t s = now;
    uint64_t st = s | (btn_stable & recent);
    if (st != btn_stable) {
        uint64_t diff = st ^ btn_stable;
        btn_stable = st;
        s = st;
        /* Who is live as the pass starts: a press that switches a layer
         * isn't taken again by the twin it makes live. */
        uint64_t live = BTN_LIVE_ALL;
        for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
            if (!((diff >> i) & 1u))
                continue;
            int down = (s >> i) & 1u;
            if (gm_safety_button(i, down))
                continue;                     /* an unlock / factory-reset combo's (gm_safety.h) */
            if (down ? !((live >> i) & 1u) : !((btn_held >> i) & 1u))
                continue;                     /* the press is the live twin's (or a hidden one's release) */
            if (down)
                btn_held |= 1ull << i;
            else
                btn_held &= ~(1ull << i);
            if ((btn_ignore >> i) & 1u) {
                btn_ignore &= ~(1ull << i);   /* released after a preset switch */
#if GM_N_MODES > 0
                mode_late_release(i);
#endif
                continue;
            }
#if GM_N_MODES > 0
            if (mode_button(i, down))
                continue;
            if (mode_btn(i, down))
                continue;
#endif
            button_changed(i, down);
        }
    }
}

/* Called once per elapsed millisecond. */
/* The buttons set to cc_ramp, worked out again when a param changes: the
 * millisecond loop looks at those only. */
static btn_bits_t ramp_mask;
static uint32_t ramp_gen;

void ramps_ms(uint32_t now)
{
    if (ramp_gen != params_gen || !params_gen) {
        ramp_gen = params_gen;
        ramp_mask = 0;
        for (uint32_t i = 0; i < GM_N_BUTTONS; i++)
            if (gm_cfg.btn_action[i] == ACT_CC_RAMP)
                ramp_mask |= (btn_bits_t)1u << i;
    }
    for (btn_bits_t m = ramp_mask; m; m &= m - 1u) {
        uint32_t i = (uint32_t)__builtin_ctzll(m);
        int32_t target = ((btn_held >> i) & 1u) ? (127 << 8) : 0;
        int32_t step = gm_cfg.ramp_time > 0 ? (127 << 8) / gm_cfg.ramp_time : (127 << 8);
        if (step < 1)
            step = 1;
        if (ramp_q8[i] < target)
            ramp_q8[i] = (int16_t)(ramp_q8[i] + step > target ? target : ramp_q8[i] + step);
        else if (ramp_q8[i] > target)
            ramp_q8[i] = (int16_t)(ramp_q8[i] - step < target ? target : ramp_q8[i] - step);
        int16_t v = (int16_t)(ramp_q8[i] >> 8);
        if (v != ramp_sent[i]) {
            ramp_sent[i] = v;
            BTN_CC(i, (uint32_t)gm_cfg.btn_number[i], (uint32_t)v);
        }
    }
    int32_t step = gm_cfg.bend_glide > 0 ? 8192 / gm_cfg.bend_glide : 16384;
    if (step < 1)
        step = 1;
    if (bend < bend_target)
        bend = bend + step > bend_target ? bend_target : bend + step;
    else if (bend > bend_target)
        bend = bend - step < bend_target ? bend_target : bend - step;
    static uint32_t t_bend;
    if (bend != bend_sent && (now - t_bend >= BEND_SEND_MS || bend == bend_target)) {
        t_bend = now;
        bend_sent = bend;
        uint32_t v = (uint32_t)(bend + 8192);
        midi3(0xE0 | midi_chan(), v & 0x7F, v >> 7);
    }
}

void buttons_init(void)
{
    for (uint32_t i = 0; i < NB; i++) {
        btn_note[i] = NOTE_NONE;
        ramp_sent[i] = 0;
    }
}

void buttons_off(void)
{
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
        int held = (btn_held >> i) & 1u;
        int32_t act = gm_cfg.btn_action[i];
        if (btn_note[i] != NOTE_NONE) {
            midi3(0x80 | btn_chan[i], btn_note[i], 0);
            btn_note[i] = NOTE_NONE;
        }
        if (held && act == ACT_CC_MOMENTARY)
            BTN_CC(i, (uint32_t)gm_cfg.btn_number[i], 0);
#if GM_MCU
        if (held && act == ACT_MCU)
            gm_mcu_note((uint32_t)gm_cfg.btn_number[i], 0);
#endif
        if (act == ACT_CC_RAMP && ramp_sent[i] != 0)
            BTN_CC(i, (uint32_t)gm_cfg.btn_number[i], 0);
        ramp_q8[i] = 0;
        ramp_sent[i] = 0;
    }
    btn_ignore = btn_held;
    btn_toggled = 0;
    if (bend_sent != 0x7FFFFFFF && bend_sent != 0)
        midi3(0xE0 | midi_chan(), 0, 0x40);      /* centre */
    bend = bend_target = 0;
    bend_sent = 0;
}
#endif
