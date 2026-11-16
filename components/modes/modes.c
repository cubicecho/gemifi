/*
 * modes.c: the modes (modes:), a control component (README.md here): key
 * and button modes, hold modes, layers and cycles. The buttons hand a mode
 * its buttons (mode_toggle_button, mode_function_button), the key matrix
 * its keys (keys_mode, mode_key); the LEDs show a mode's LEDs (mode_led).
 * layers_update picks the live one of each control and its layer twins.
 * Shares its state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_display.h"
#include "gm_presets.h"
#include "gm_hal.h"

#if GM_N_MODES > 0
/* Which modes are on (components/modes/README.md "Modes", "Cycles"): the key modes
 * (keys:, buttons:, sub-modes) share mode_cur, one at a time; a held hold
 * mode is hold_cur. With layers or cycles (GM_LAYER_PARAMS), each
 * layer-only toggle mode has its own on/off (solo_on) and each cycle its
 * current step (cyc_cur), so any of them can be on together. */
static uint8_t mode_cur = GM_MODE_NONE;     /* the active toggled key mode; GM_MODE_NONE = none */
static uint8_t hold_cur = GM_MODE_NONE;     /* the hold mode whose button is held (or a one-shot) */
#if GM_MODE_ONE_SHOT
/* One-shots (components/modes/README.md "One-shot layers"), per mode: bit m of
 * shot_fresh, one-shot m's button is down and no other button has been
 * pressed since (let go now, it arms). shot_armed: hold_cur is a one-shot
 * whose button was let go fresh, on until the next press of another
 * button (every press goes to mode_one_shot_press first, so hold_cur
 * can't move on while it is armed). */
static uint64_t shot_fresh;
static uint8_t shot_armed;

/* The one-shot modes button i arms, one bit each. */
static uint64_t one_shots_of(uint32_t i)
{
    uint64_t of = 0;
    for (uint32_t m = 0; m < GM_N_MODES; m++)
        if (gm_modes[m].hold == GM_HOLD_ONE_SHOT && gm_modes[m].toggle == i)
            of |= 1ull << m;
    return of;
}
#endif
#if GM_LAYER_PARAMS
static uint64_t solo_mask;          /* layer-only toggle modes (bit = mode index), modes_init */
/* LEDs a cycle step or a layer-only toggle lights (bit = LED), modes_init:
 * mode_led() looks through the modes only for these. */
static uint32_t solo_leds[(GM_ARR(GM_N_LEDS) + 31u) / 32u];
static uint64_t solo_on;            /* ... the ones that are on */
static uint64_t modes_was;          /* modes on at the last layers_update: EVENT 8 on a change */
static uint8_t report_due;          /* gm_layers_report: the EVENT 8 replay, from gm_poll */
#if GM_N_CYCLES
static uint8_t cyc_cur[GM_N_CYCLES];   /* each cycle's current step (mode index) */
#endif
#endif

#define SHOW_STEP_MS 200u           /* preset number blink: one step */
#define MODE_BLINK_MS 250u          /* a blinking mode LED: half period */
static uint8_t show_n;              /* slot being shown on the LEDs, 0 = none */
static uint8_t show_mode;           /* the mode whose LEDs show it */
static uint32_t show_t0;
static uint64_t btn_in_mode;        /* pressed as a mode's function button: its release is silent */

#if GM_HAS_MATRIX
/* The mode the keys go to: a held hold mode with keys, else the toggled
 * one if it has keys; GM_MODE_NONE = the keys play. */
uint32_t keys_mode(void)
{
    if (hold_cur != GM_MODE_NONE && gm_modes[hold_cur].n_keys)
        return hold_cur;
    if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].n_keys)
        return mode_cur;
    return GM_MODE_NONE;
}

/* Mode md is about to start: if it takes the keys and no mode had them,
 * the held keys get their Note Off first (their releases send nothing). */
static void keys_to_mode(const gm_mode_t *md)
{
    if (md->n_keys && keys_mode() == GM_MODE_NONE)
        keys_off();
}
#else
#define keys_to_mode(md) ((void)(md))   /* no key matrix: no mode has keys (the builder checks) */
#endif

/* "CH <n>" on the display. */
static void show_channel(void)
{
    char t[6] = "CH ";
    uint32_t c = (uint32_t)gm_cfg.channel, j = 3;
    if (c >= 10u)
        t[j++] = (char)('0' + c / 10u);
    t[j++] = (char)('0' + c % 10u);
    t[j] = 0;
    gm_display_message(t);
}

static int binds_channel(const gm_mode_key_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (b[i].op == GM_MK_CHANNEL || b[i].op == GM_MK_CHANNEL_UP || b[i].op == GM_MK_CHANNEL_DOWN)
            return 1;
    return 0;
}

#if GM_LAYER_PARAMS
/* How strongly root mode m's layer holds, 0 = off. When layers on at once
 * have twins of the same component, the strongest wins: the held hold
 * mode, then the toggled key mode, then cycle steps, then layer-only
 * toggles, each in mode order (components/modes/README.md "Cycles"). */
static uint32_t layer_rank(uint32_t m)
{
    if (hold_cur == m)
        return 255u;
    if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].root == m)
        return 254u;
#if GM_N_CYCLES
    uint32_t g = gm_modes[m].cycle;
    if (g && cyc_cur[g - 1u] == m)
        return 200u - m;            /* GM_N_MODES <= 64 */
#endif
    if ((solo_on >> m) & 1u)
        return 128u - m;
    return 0;
}

#if GM_N_CYCLES
/* The step of cycle g (1..) after mode from: the next enabled one, round
 * the group; GM_MODE_NONE: the first enabled one (power-on). Every step
 * disabled: the first step. */
static uint32_t cycle_after(uint32_t g, uint32_t from)
{
    uint32_t first = 0;
    while (first < GM_N_MODES - 1u && gm_modes[first].cycle != g)
        first++;
    uint32_t s = from == GM_MODE_NONE ? first + GM_N_MODES - 1u : from;
    for (uint32_t k = 1; k <= GM_N_MODES; k++) {
        uint32_t m = (s + k) % GM_N_MODES;
        if (gm_modes[m].cycle == g && gm_cfg.layer_enabled[m])
            return m;
    }
    return first;
}

/* The cycle button: the next enabled step's layer, its name on the
 * display, its colour on the cycle's LED (mode_led), EVENT 8 for the old
 * step (0) and the new one (1) (layers_update). */
static void cycle_press(uint32_t g)
{
    uint32_t m = cycle_after(g, cyc_cur[g - 1u]);
    cyc_cur[g - 1u] = (uint8_t)m;
    layers_update();
    if (gm_modes[m].name)
        gm_display_message(gm_modes[m].name);
}
#endif
#endif

/* Another hold mode whose button is still held (two held at once: letting
 * go of one goes back to the other), or GM_MODE_NONE. */
static uint8_t hold_still_held(uint32_t except)
{
    for (uint32_t m = 0; m < GM_N_MODES; m++)
        if (m != except && gm_modes[m].hold && gm_modes[m].toggle < GM_N_BUTTONS &&
            ((btn_held >> gm_modes[m].toggle) & 1u)
#if GM_LAYER_PARAMS
            && gm_cfg.layer_enabled[m]
#endif
            )
            return (uint8_t)m;
    return GM_MODE_NONE;
}

#if GM_MODE_ONE_SHOT
/* The button of one-shot mode m: held, the mode is on as a hold mode is;
 * let go before another button was pressed, it stays on for that press
 * (mode_one_shot_press ends it); pressed again first, it is off. */
static int one_shot_button(uint32_t m, uint32_t i, int down)
{
    uint64_t bit = 1ull << m;
    int is_on = hold_cur == m;
    int was_fresh = (shot_fresh & bit) != 0;
    if (!down)
        shot_fresh &= ~bit;
    if (down && is_on && shot_armed) {
        shot_armed = 0;             /* pressed again: off */
        hold_cur = hold_still_held(m);
    } else if (down) {
        hold_cur = (uint8_t)m;
        shot_fresh |= bit;
    } else if (is_on && was_fresh) {
        shot_armed = 1;             /* let go alone: on for the next press */
        app_log(LOG_DEBUG, gm_device.buttons[i].name, ": one shot");
        return 1;
    } else if (is_on) {
        hold_cur = hold_still_held(m);   /* used while held: off with its button */
    } else {
        return 1;                   /* another mode took over, or the press that turned it off */
    }
    layers_update();
    app_log(LOG_DEBUG, gm_device.buttons[i].name, hold_cur == m ? ": mode on" : ": mode off");
    return 1;
}

void mode_one_shot_press(uint32_t i)
{
    shot_fresh &= one_shots_of(i);  /* the others' buttons are held while another is pressed */
    int takes_the_layer = shot_armed && gm_modes[hold_cur].toggle != i;
    if (takes_the_layer) {
        shot_armed = 0;             /* this press took the layer: back */
        hold_cur = hold_still_held(hold_cur);
        layers_update();
    }
}
#endif

/* The toggle or hold button of a root mode. Toggle: enters the mode (held
 * keys get their Note Off first, if it has keys), or leaves it from the
 * mode or any of its sub-modes. Hold: the mode is active while the button
 * is down. Its own action never runs, except a modifier's (a hold mode
 * with only a layer, like Shift): that switches its layer and returns 0,
 * so the button's action runs too. Returns 0 if button i is neither.
 * A twin of a toggle: { button, with } button toggles the mode as well.
 * A cycle's button (on each of its steps) moves it to its next step. A
 * disabled mode (layer.enabled 0) can't be entered: its button is as if
 * it weren't there (it can still be left). */
int mode_toggle_button(uint32_t i, int down)
{
#if GM_LAYERS
    const gm_button_t *bt = &gm_device.buttons[i];
    uint32_t base = bt->layer ? bt->base : i;
#else
    uint32_t base = i;
#endif
    for (uint32_t m = 0; m < GM_N_MODES; m++) {
        const gm_mode_t *md = &gm_modes[m];
        if (md->toggle != base || (base != i && !md->with))
            continue;
#if GM_LAYER_PARAMS
        if (!md->cycle && !gm_cfg.layer_enabled[m] && !layer_rank(m) && !(btn_in_mode & (1ull << i)))
            continue;
#endif
        if (md->with && !md->hold) {
            /* toggle: { button, with }: only with that button held; a plain
             * press is the button's own (its release too). */
            uint64_t bit = 1ull << i;
            if (down ? !((btn_held >> (md->with - 1u)) & 1u) : !(btn_in_mode & bit))
                return 0;
            if (down)
                btn_in_mode |= bit;
            else
                btn_in_mode &= ~bit;
        }
        if (!md->modifier)
            send_event(EV_BUTTON, i, (uint32_t)down);    /* a modifier's: button_changed */
#if GM_N_CYCLES
        if (md->cycle) {
            if (down)
                cycle_press(md->cycle);
            return 1;
        }
#endif
#if GM_MODE_ONE_SHOT
        if (md->hold == GM_HOLD_ONE_SHOT)
            return one_shot_button(m, i, down);
#endif
        if (md->hold) {
            if (down) {
                keys_to_mode(md);
                hold_cur = (uint8_t)m;
                if (binds_channel(md->buttons, md->n_buttons) || binds_channel(md->keys, md->n_keys))
                    show_channel();
            } else if (hold_cur == m) {
                hold_cur = hold_still_held(m);
            }
            layers_update();
            app_log(LOG_DEBUG, gm_device.buttons[i].name, down ? ": mode on" : ": mode off");
            return !md->modifier;
        }
        if (!down)
            return 1;
#if GM_LAYER_PARAMS
        if ((solo_mask >> m) & 1u) {
            solo_on ^= 1ull << m;           /* a layer of its own: the others stay */
            layers_update();
            app_log(LOG_DEBUG, gm_device.buttons[i].name, ((solo_on >> m) & 1u) ? ": layer on" : ": layer off");
            return 1;
        }
#endif
        if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].root == m) {
            mode_cur = GM_MODE_NONE;
        } else {
            keys_to_mode(md);
            mode_cur = (uint8_t)m;
        }
        layers_update();
        app_log(LOG_DEBUG, gm_device.buttons[i].name, mode_cur == GM_MODE_NONE ? ": mode off" : ": mode on");
        return 1;
    }
    return 0;
}

/* A device-side change of a per-preset param: the host hears of it, and the
 * preset is now dirty (like a host edit: COMMIT keeps it, a switch drops it). */
static void set_live(int32_t *v, int32_t x, uint16_t id)
{
    if (*v == x)
        return;
    *v = x;
    app_param_changed(id, 0);
    presets_host_set(id, 0);
}

/* Run a binding of mode mi (a key's or a button's), then a sub-mode goes
 * back to its parent. */
static void mode_run(uint32_t mi, const gm_mode_key_t *b)
{
    const gm_mode_t *m = &gm_modes[mi];
    int up = 1;
    switch (b->op) {
    case GM_MK_MODE:
        mode_cur = b->arg;
        return;
    case GM_MK_PRESET:
    case GM_MK_PRESET_NEXT:
    case GM_MK_PRESET_PREV:
        if (b->op == GM_MK_PRESET)
            presets_select(b->arg);
        else
            presets_step(b->op == GM_MK_PRESET_NEXT ? 1 : -1);
        show_n = (uint8_t)presets_active();
        show_mode = (uint8_t)mi;
        show_t0 = hal_millis();
        break;
    case GM_MK_OCTAVE_DOWN:
        up = -1;
        /* fall through */
    case GM_MK_OCTAVE_UP:
    case GM_MK_OCTAVE_RESET:
        step_and_show(&gm_cfg.octave, up, 4, P_OCTAVE, b->op == GM_MK_OCTAVE_RESET);
        break;
    case GM_MK_TRANSPOSE_DOWN:
        up = -1;
        /* fall through */
    case GM_MK_TRANSPOSE_UP:
    case GM_MK_TRANSPOSE_RESET:
        step_and_show(&gm_cfg.transpose, up, 24, P_TRANSPOSE, b->op == GM_MK_TRANSPOSE_RESET);
        break;
    case GM_MK_CHANNEL:
    case GM_MK_CHANNEL_UP:
    case GM_MK_CHANNEL_DOWN: {
        int32_t c = b->op == GM_MK_CHANNEL ? b->arg
                  : b->op == GM_MK_CHANNEL_UP ? gm_cfg.channel % 16 + 1
                  : (gm_cfg.channel + 14) % 16 + 1;
        set_live(&gm_cfg.channel, c, P_CHANNEL);
        show_channel();
        break;
    }
    case GM_MK_CURVE:
        set_live(&gm_cfg.vel_curve, b->arg, P_VEL_CURVE);
        break;
    default:
        break;
    }
    if (m->parent != GM_MODE_NONE && mode_cur == mi)
        mode_cur = m->parent;
}

#if GM_HAS_MATRIX
/* A key pressed in mode mi: run its binding (unbound keys do nothing). */
void mode_key(uint32_t mi, uint32_t k)
{
    const gm_mode_t *m = &gm_modes[mi];
    for (uint32_t i = 0; i < m->n_keys; i++)
        if (m->keys[i].key == k) {
            mode_run(mi, &m->keys[i]);
            return;
        }
}
#endif

/* Button i pressed or released while a mode is active: a button the mode
 * binds runs that instead of its own action (the held hold mode first, then
 * the toggled one), and its release is silent, even after the mode ends.
 * Returns 1 if the mode took the change. */
int mode_function_button(uint32_t i, int down)
{
    uint64_t bit = 1ull << i;
    if (!down) {
        if (!(btn_in_mode & bit))
            return 0;
        btn_in_mode &= ~bit;
        send_event(EV_BUTTON, i, 0);
        return 1;
    }
    btn_in_mode &= ~bit;
    const uint8_t cand[2] = { hold_cur, mode_cur };
    for (uint32_t c = 0; c < 2; c++) {
        uint32_t mi = cand[c];
        if (mi == GM_MODE_NONE)
            continue;
        const gm_mode_t *m = &gm_modes[mi];
        for (uint32_t k = 0; k < m->n_buttons; k++)
            if (m->buttons[k].key == i) {
                btn_in_mode |= bit;
                send_event(EV_BUTTON, i, 1);
                app_log(LOG_DEBUG, gm_device.buttons[i].name, ": mode action");
                mode_run(mi, &m->buttons[k]);
                return 1;
            }
    }
    return 0;
}

/* An int8_t LED reference (-1 = none) is LED li. Compared unsigned: a
 * device has up to 255 LEDs, and (int8_t)li is negative from 128 on. */
#define LED_IS(ref, li) ((ref) >= 0 && (uint32_t)(ref) == (li))

/* What LED li shows instead of level: after a preset switch from a mode,
 * its count LED blinks the slot number, then it and the done LED flash
 * once; while a mode is active (or its hold button held), its LED is lit
 * (or blinks). */
uint32_t mode_led(uint32_t li, uint32_t level, uint32_t now)
{
    if (show_n) {
        const gm_mode_t *m = &gm_modes[show_mode];
        uint32_t s = (now - show_t0) / SHOW_STEP_MS, n2 = 2u * show_n;
        int is_count = LED_IS(m->blink_count, li), is_done = LED_IS(m->blink_done, li);
        if (s > n2 + 2u)
            show_n = 0;             /* done: a dark step, N blinks, a 2-step flash */
        else if (is_count || is_done)
            return s > n2 ? 1u : is_count && (s & 1u);
    }
    if (hold_cur != GM_MODE_NONE && LED_IS(gm_modes[hold_cur].led, li))
        return gm_modes[hold_cur].led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
    if (mode_cur != GM_MODE_NONE && LED_IS(gm_modes[mode_cur].led, li))
        return gm_modes[mode_cur].led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
#if GM_LAYER_PARAMS
    if (!((solo_leds[li >> 5] >> (li & 31u)) & 1u))
        return level;
    for (uint32_t m = 0; m < GM_N_MODES; m++) {     /* cycle steps and layer-only toggles */
        const gm_mode_t *md = &gm_modes[m];
        if (!LED_IS(md->led, li) || !(md->cycle || ((solo_mask >> m) & 1u)) || !layer_rank(m))
            continue;
        if (md->colour) {
#if GM_MODE_RGB
            if (gm_mode_rgb[m] && m < 127u)
                return LV_RGB | LV_STEP(m) | (md->colour & 7u);  /* its full colour, where it shows */
#endif
            return LV_RGB | (md->colour & 7u);       /* the step's colour */
        }
        return md->led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
    }
#endif
    return level;
}

void mode_late_release(uint32_t i)
{
#if GM_MODE_ONE_SHOT
    shot_fresh &= ~one_shots_of(i); /* held through a preset switch: it ends, unarmed */
#endif
    if (hold_cur != GM_MODE_NONE && gm_modes[hold_cur].toggle == i) {
        hold_cur = hold_still_held(hold_cur);  /* a hold mode still ends with its button */
        layers_update();
    }
    btn_in_mode &= ~(1ull << i);
}

#if GM_LAYER_PARAMS
/* Which of each base and its twins is live (components/modes/README.md "Button layers"):
 * the twin of the strongest layer that is on (layer_rank), else the base.
 * An LED that changes hands is drawn again. Then EVENT 8 for each mode
 * that went off (0), then each that came on (1). */
void layers_update(void)
{
#if GM_LAYERS
#define RANK(layer) ((layer) ? layer_rank((layer) - 1u) : 0u)
#if GM_PAD_TWINS
    uint8_t pr[GM_N_PADS];
    for (uint32_t i = 0; i < GM_N_PADS; i++)
        pad_live[i] = pr[i] = 0;
    for (uint32_t t = 0; t < GM_PAD_TWINS; t++) {
        uint32_t b = gm_pad_twins[t].base, r = RANK(gm_pad_twins[t].layer);
        if (r > pr[b]) {
            pr[b] = (uint8_t)r;
            pad_live[b] = (uint8_t)(t + 1u);
        }
    }
#endif
#if GM_ENC_TWINS
    uint8_t er[NE_HW];
    for (uint32_t i = 0; i < NE_HW; i++) {
        enc_live[i] = (uint8_t)i;
        er[i] = 0;
    }
    for (uint32_t i = NE_HW; i < GM_N_ENCODERS; i++) {
        uint32_t b = gm_device.encoders[i].base, r = RANK(gm_device.encoders[i].layer);
        if (r > er[b]) {
            er[b] = (uint8_t)r;
            enc_live[b] = (uint8_t)i;
        }
    }
#endif
    /* buttons: the twins are the last GM_BTN_TWINS */
    uint8_t br[NB], bl[NB];
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++) {
        br[i] = 0;
        bl[i] = (uint8_t)i;
    }
    for (uint32_t i = GM_N_OWN_BUTTONS; i < GM_N_BUTTONS; i++) {
        uint32_t b = gm_device.buttons[i].base, r = RANK(gm_device.buttons[i].layer);
        if (r > br[b]) {
            br[b] = (uint8_t)r;
            bl[b] = (uint8_t)i;
        }
    }
    uint64_t live = 0;
    for (uint32_t i = 0; i < GM_N_OWN_BUTTONS && i < 64; i++)
        live |= 1ull << bl[i];
    btn_live = live;
    uint8_t lr[GM_ARR(GM_N_LEDS)], ll[GM_ARR(GM_N_LEDS)];
    for (uint32_t i = 0; i < GM_N_LEDS; i++) {
        lr[i] = 0;
        ll[i] = (uint8_t)i;
    }
    for (uint32_t i = 0; i < GM_N_LEDS; i++) {
        const gm_led_t *l = &gm_device.leds[i];
        uint32_t r = RANK(l->layer);
        if (l->layer && r > lr[l->base]) {
            lr[l->base] = (uint8_t)r;
            ll[l->base] = (uint8_t)i;
        }
    }
    for (uint32_t i = 0; i < GM_N_LEDS; i++) {
        const gm_led_t *l = &gm_device.leds[i];
        uint8_t dead = ll[l->layer ? l->base : i] != i;
        if (led_dead[i] != dead) {
            led_dead[i] = dead;
            led_redraw(i);              /* drawn again once it is live */
        }
    }
#undef RANK
#endif
    uint64_t on = 0;
    for (uint32_t m = 0; m < GM_N_MODES; m++)
        if (layer_rank(m))
            on |= 1ull << m;
    uint64_t d = on ^ modes_was;
    modes_was = on;
    for (uint32_t v = 0; v < 2u; v++)
        for (uint32_t m = 0; m < GM_N_MODES; m++)
            if (((d >> m) & 1u) && ((on >> m) & 1u) == v)
                send_event(EV_LAYER, m, v);
}
#endif

void gm_layers_report(void)
{
#if GM_LAYER_PARAMS
    report_due = 1;
#endif
}

#if GM_LAYER_PARAMS
void layers_report_poll(void)
{
    if (report_due) {               /* debug.events set to 1: every mode that is on */
        report_due = 0;
        for (uint32_t m = 0; m < GM_N_MODES; m++)
            if ((modes_was >> m) & 1u)
                send_event(EV_LAYER, m, 1);
    }
}
#endif

void modes_init(void)
{
    mode_cur = hold_cur = GM_MODE_NONE;
    show_n = 0;
    btn_in_mode = 0;
#if GM_MODE_ONE_SHOT
    shot_fresh = 0;
    shot_armed = 0;
#endif
#if GM_LAYER_PARAMS
    /* layer-only toggles: no keys or buttons (so no sub-modes either) */
    solo_mask = solo_on = 0;
    for (uint32_t m = 0; m < GM_N_MODES; m++) {
        const gm_mode_t *md = &gm_modes[m];
        if (md->toggle != GM_MODE_NONE && !md->hold && !md->cycle && !md->n_keys && !md->n_buttons)
            solo_mask |= 1ull << m;
    }
    for (uint32_t w = 0; w < sizeof solo_leds / sizeof solo_leds[0]; w++)
        solo_leds[w] = 0;
    for (uint32_t m = 0; m < GM_N_MODES; m++) {
        const gm_mode_t *md = &gm_modes[m];
        if (md->led >= 0 && (uint32_t)md->led < gm_device.n_leds && (md->cycle || ((solo_mask >> m) & 1u)))
            solo_leds[(uint32_t)md->led >> 5] |= 1u << ((uint32_t)md->led & 31u);
    }
#if GM_N_CYCLES
    for (uint32_t g = 0; g < GM_N_CYCLES; g++)
        cyc_cur[g] = GM_MODE_NONE;
#endif
#endif
}

#if GM_N_CYCLES
void cycles_start(void)
{
    for (uint32_t g = 0; g < GM_N_CYCLES; g++)
        cyc_cur[g] = (uint8_t)cycle_after(g + 1u, GM_MODE_NONE);   /* the first enabled step */
}
#endif
#endif
