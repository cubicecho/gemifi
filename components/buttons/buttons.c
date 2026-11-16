/*
 * buttons.c: the buttons (buttons:), a control component (README.md here):
 * sampling and debouncing them, their actions, the CC ramps and the pitch
 * bend buttons' glide. The main loop (core/src/controller.c gm_poll) calls
 * buttons_sample and ramps_ms; a button a mode takes goes to the modes
 * (mode_toggle_button, mode_function_button). Shares its state through
 * gm_controls.h.
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
#include "gm_hal.h"
#include "params.h"

#if GM_N_BUTTONS > 0
#define BEND_SEND_MS 2u             /* pitch bend buttons: at most one bend per 2 ms while it glides */
uint64_t btn_stable, btn_toggled;
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

void step_and_show(int32_t *v, int32_t step, int32_t lim, uint16_t id, int reset)
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

/* Action act with number num of button i, pressed (down) or let go: the
 * button's own action, or one of its gestures'. It sends with the button's
 * channel:, min:, max: and bank:, and keeps its note and toggle state in
 * the button's. */
static void run_action(uint32_t i, int32_t act, uint32_t num, int down)
{
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
            step_and_show(&gm_cfg.octave, act == ACT_OCTAVE_UP ? 1 : -1, 4, P_OCTAVE, other);
        }
        break;
    case ACT_TRANSPOSE_DOWN:
    case ACT_TRANSPOSE_UP:
        if (down) {
            int other = held_with_action(act == ACT_TRANSPOSE_UP ? ACT_TRANSPOSE_DOWN : ACT_TRANSPOSE_UP, i);
            step_and_show(&gm_cfg.transpose, act == ACT_TRANSPOSE_UP ? 1 : -1, 24, P_TRANSPOSE, other);
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
            step_and_show(&gm_cfg.pad_octave, act == ACT_PAD_OCTAVE_UP ? 1 : -1, 4, P_PAD_OCTAVE, other);
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

/* A press or release as it happens: its EVENT and log line. */
static void report_change(uint32_t i, int down)
{
    send_event(EV_BUTTON, i, (uint32_t)down);
    app_log(LOG_DEBUG, gm_device.buttons[i].name, down ? " down" : " up");
}

static void button_changed(uint32_t i, int down)
{
    int32_t act = gm_cfg.btn_action[i];
    uint32_t num = (uint32_t)gm_cfg.btn_number[i];
    report_change(i, down);
    run_action(i, act, num, down);
}

#if GM_BTN_GESTURES
/* Gestures (components/buttons/README.md "Gestures"). A button with one,
 * or in a combo, doesn't run its action at the press: gesture_input takes
 * its presses and releases, and gestures_ms (every sample) times them. The
 * other buttons never wait. */
#define GESTURE_BUTTONS ((btn_bits_t)(GM_BTN_HOLD | GM_BTN_DOUBLE | GM_COMBO_BUTTONS))
#define HAS(mask, i) (((btn_bits_t)(mask) >> (i)) & 1u)
#if GM_BTN_HOLD
#define HOLD_MS ((uint32_t)gm_cfg.btn_hold_time)
#else
#define HOLD_MS 0u                  /* no button has hold: */
#endif
#if GM_BTN_DOUBLE
#define TAP_MS ((uint32_t)gm_cfg.btn_tap_time)
#else
#define TAP_MS 0u                   /* no button has double_tap: */
#endif
enum {
    GS_IDLE,                        /* up, nothing to decide */
    GS_COMBO_WAIT,                  /* down, held back: it may be part of a combo */
    GS_PRESSED,                     /* down, not told apart yet */
    GS_TAPPED,                      /* let go: a tap, unless pressed again within tap_time */
    GS_OWN,                         /* its own action is down */
    GS_HOLD,                        /* its hold action is down */
    GS_DOUBLE,                      /* its double_tap action is down */
    GS_COMBO,                       /* down, in a combo whose action is down */
    GS_SILENT,                      /* down, in a combo already let go: its release sends nothing */
};
static uint8_t gs_state[NB];
static uint32_t gs_since[NB];       /* ms: the press, or the release (GS_TAPPED) */

/* The button's own action (button.action, button.number). */
static void own_action(uint32_t i, int down)
{
    run_action(i, gm_cfg.btn_action[i], (uint32_t)gm_cfg.btn_number[i], down);
}

static void gesture_action(uint32_t i, const gm_btn_act_t *a, int down)
{
    run_action(i, a->action, a->number, down);
}

/* A tap: the button's own action, pressed and let go. */
static void tap(uint32_t i)
{
    own_action(i, 1);
    own_action(i, 0);
}

#if GM_N_COMBOS
/* Combos (components/buttons/README.md "Combos"): presses of their buttons
 * wait in combo_queue, in order, until they are a combo or can't be one. */
static btn_bits_t combo_waiting;
static uint8_t combo_queue[GM_N_COMBO_BUTTONS];
static uint8_t combo_n;
static uint32_t combo_since;        /* ms: the first press waiting */
static uint32_t combo_down;         /* bit c: combo c's action is down */

/* A combo's action runs on its first button: that button's channel and note. */
#define COMBO_OWNER(c) ((uint32_t)__builtin_ctzll(gm_btn_combos[c].buttons))

/* The combo whose buttons are exactly set, or -1; *more: a combo with more
 * buttons than set, set among them. */
static int combo_match(btn_bits_t set, int *more)
{
    int exact = -1;
    *more = 0;
    for (uint32_t c = 0; c < GM_N_COMBOS; c++) {
        btn_bits_t m = (btn_bits_t)gm_btn_combos[c].buttons;
        if (m == set)
            exact = (int)c;
        else if ((m & set) == set)
            *more = 1;
    }
    return exact;
}

static void combo_fire(uint32_t c)
{
    btn_bits_t m = (btn_bits_t)gm_btn_combos[c].buttons;
    for (btn_bits_t k = m; k; k &= k - 1u)
        gs_state[__builtin_ctzll(k)] = GS_COMBO;
    combo_waiting = 0;
    combo_n = 0;
    combo_down |= 1u << c;
    app_log(LOG_DEBUG, gm_device.buttons[COMBO_OWNER(c)].name, ": combo");
    gesture_action(COMBO_OWNER(c), &gm_btn_combos[c].act, 1);
}

/* Combo member i let go: the first of its buttons lets go of the combo's
 * action, the others' releases are silent. */
static void combo_up(uint32_t i)
{
    for (uint32_t c = 0; c < GM_N_COMBOS; c++) {
        btn_bits_t m = (btn_bits_t)gm_btn_combos[c].buttons;
        int is_its_combo = ((combo_down >> c) & 1u) && HAS(m, i);
        if (!is_its_combo)
            continue;
        combo_down &= ~(1u << c);
        for (btn_bits_t k = m; k; k &= k - 1u) {
            uint32_t j = (uint32_t)__builtin_ctzll(k);
            if (gs_state[j] == GS_COMBO)
                gs_state[j] = GS_SILENT;
        }
        gesture_action(COMBO_OWNER(c), &gm_btn_combos[c].act, 0);
        return;
    }
}
#endif

/* Let go of the action state s has down on button i. */
static void let_go(uint32_t i, uint8_t s)
{
    switch (s) {
    case GS_OWN:
        own_action(i, 0);
        break;
    case GS_HOLD:
        gesture_action(i, &gm_btn_gestures[i].hold, 0);
        break;
    case GS_DOUBLE:
        gesture_action(i, &gm_btn_gestures[i].double_tap, 0);
        break;
#if GM_N_COMBOS
    case GS_COMBO:
        combo_up(i);
        break;
#endif
    default:                        /* nothing down */
        break;
    }
}

/* Button i's press, now that no combo takes it (pressed at gs_since[i]):
 * a button with hold: or double_tap: waits to be told apart, a combo's
 * other buttons run their action. */
static void gesture_start(uint32_t i)
{
    if (HAS(GM_BTN_HOLD | GM_BTN_DOUBLE, i)) {
        gs_state[i] = GS_PRESSED;
        return;
    }
    gs_state[i] = GS_OWN;
    own_action(i, 1);
}

#if GM_N_COMBOS
/* The presses waiting for a combo aren't one: each goes on as a press of
 * its own, in order. A preset switch from one of them drops the rest. */
static void combo_flush(void)
{
    uint8_t queue[GM_N_COMBO_BUTTONS];
    uint32_t n = combo_n;
    for (uint32_t k = 0; k < n; k++)
        queue[k] = combo_queue[k];
    combo_waiting = 0;
    combo_n = 0;
    for (uint32_t k = 0; k < n; k++)
        if (gs_state[queue[k]] == GS_COMBO_WAIT)
            gesture_start(queue[k]);
}

/* Decide the presses waiting: a combo if they are exactly one, else each
 * on its own. */
static void combo_settle(void)
{
    int more;
    int exact = combo_match(combo_waiting, &more);
    if (exact >= 0)
        combo_fire((uint32_t)exact);
    else
        combo_flush();
}

/* Combo button i pressed: it waits with the others, and they are a combo
 * as soon as no bigger one could still come. What waits but can't make a
 * combo with i is decided first. */
static void combo_press(uint32_t i, uint32_t now)
{
    btn_bits_t bit = (btn_bits_t)1 << i;
    int more;
    int exact = combo_match(combo_waiting | bit, &more);
    int cannot_join = exact < 0 && !more;
    if (cannot_join && combo_waiting) {
        combo_settle();
        exact = combo_match(bit, &more);
    }
    if (!combo_waiting)
        combo_since = now;
    combo_waiting |= bit;
    combo_queue[combo_n++] = (uint8_t)i;
    gs_state[i] = GS_COMBO_WAIT;
    gs_since[i] = now;
    if (exact >= 0 && !more)
        combo_fire((uint32_t)exact);
}
#endif

#if GM_BTN_DOUBLE || GM_N_COMBOS
/* Button i is about to be pressed: a tap still waiting for its second
 * press (another button's, or i's own after tap_time) is a tap, and a
 * combo i isn't in is decided; both go out first. Called before i is
 * held, so a preset switch from them leaves i's press its own. */
static void gestures_before_press(uint32_t i, uint32_t now)
{
#if GM_BTN_DOUBLE
    for (btn_bits_t m = GM_BTN_DOUBLE; m; m &= m - 1u) {
        uint32_t j = (uint32_t)__builtin_ctzll(m);
        int is_waiting = gs_state[j] == GS_TAPPED && (j != i || now - gs_since[j] >= TAP_MS);
        if (is_waiting) {
            gs_state[j] = GS_IDLE;
            tap(j);
        }
    }
#endif
#if GM_N_COMBOS
    if (combo_waiting && !HAS(GM_COMBO_BUTTONS, i))
        combo_settle();
#endif
}
#endif

static void gesture_press(uint32_t i, uint32_t now)
{
    if (gs_state[i] == GS_TAPPED) { /* again within tap_time (gestures_before_press took a later one) */
        gs_state[i] = GS_DOUBLE;
        app_log(LOG_DEBUG, gm_device.buttons[i].name, ": double tap");
        gesture_action(i, &gm_btn_gestures[i].double_tap, 1);
        return;
    }
#if GM_N_COMBOS
    if (HAS(GM_COMBO_BUTTONS, i)) {
        combo_press(i, now);
        return;
    }
#endif
    gs_since[i] = now;
    gesture_start(i);
}

static void gesture_release(uint32_t i, uint32_t now)
{
#if GM_N_COMBOS
    if (gs_state[i] == GS_COMBO_WAIT)
        combo_settle();             /* let go before the combo was decided: decide it now */
#endif
    uint8_t was = gs_state[i];
    int is_first_tap = was == GS_PRESSED && HAS(GM_BTN_DOUBLE, i);
    if (is_first_tap) {
        gs_state[i] = GS_TAPPED;
        gs_since[i] = now;
        return;
    }
    gs_state[i] = GS_IDLE;
    if (was == GS_PRESSED)          /* let go before hold_time: a tap */
        tap(i);
    else
        let_go(i, was);
}

/* Button i's press or release, if it has a gesture or is in a combo: 1 if
 * it took it. */
static int gesture_input(uint32_t i, int down, uint32_t now)
{
    if (!HAS(GESTURE_BUTTONS, i))
        return 0;
    report_change(i, down);         /* the EVENT now; the action may come later */
    if (down)
        gesture_press(i, now);
    else
        gesture_release(i, now);
    return 1;
}

/* Every sample: presses that waited combo_time for a combo are decided; a
 * press held for hold_time is a hold (a press of a button with only
 * double_tap:, held for tap_time, its own action), and a tap not pressed
 * again within tap_time a tap. The state is set before the action runs,
 * so a preset switch from it (gestures_off) has the last word. */
static void gestures_ms(uint32_t now)
{
#if GM_N_COMBOS
    int combo_timed_out = combo_waiting && now - combo_since >= (uint32_t)gm_cfg.btn_combo_time;
    if (combo_timed_out)
        combo_settle();
#endif
    for (btn_bits_t m = GESTURE_BUTTONS; m; m &= m - 1u) {
        uint32_t i = (uint32_t)__builtin_ctzll(m);
        uint32_t age = now - gs_since[i];
        int is_pressed = gs_state[i] == GS_PRESSED;
        int is_hold = is_pressed && HAS(GM_BTN_HOLD, i) && age >= HOLD_MS;
        int is_long_press = is_pressed && !HAS(GM_BTN_HOLD, i) && age >= TAP_MS;
        int is_single_tap = gs_state[i] == GS_TAPPED && age >= TAP_MS;
        if (is_hold) {
            gs_state[i] = GS_HOLD;
            app_log(LOG_DEBUG, gm_device.buttons[i].name, ": hold");
            gesture_action(i, &gm_btn_gestures[i].hold, 1);
        } else if (is_long_press) {
            gs_state[i] = GS_OWN;
            own_action(i, 1);
        } else if (is_single_tap) {
            gs_state[i] = GS_IDLE;
            tap(i);
        }
    }
}

/* Button i's release skipped the gestures (held through a preset switch):
 * nothing a gesture has down outlives it. */
static void gesture_forget(uint32_t i)
{
    if (!HAS(GESTURE_BUTTONS, i))
        return;
    uint8_t was = gs_state[i];
    gs_state[i] = GS_IDLE;
    let_go(i, was);
}

/* A preset switch: let go of what a gesture or a combo has down, and
 * forget the presses and taps still being told apart (their releases are
 * silent). Returns the buttons whose own action isn't down: buttons_off
 * lets go of the others'. */
static btn_bits_t gestures_off(void)
{
    btn_bits_t own_up = 0;
#if GM_N_COMBOS
    combo_waiting = 0;
    combo_n = 0;
#endif
    for (btn_bits_t m = GESTURE_BUTTONS; m; m &= m - 1u) {
        uint32_t i = (uint32_t)__builtin_ctzll(m);
        uint8_t was = gs_state[i];
        gs_state[i] = GS_IDLE;
        if (was == GS_OWN)
            continue;
        let_go(i, was);
        own_up |= (btn_bits_t)1 << i;
    }
    return own_up;
}
#endif

void buttons_sample(void)
{
    /* A press counts once a sample saw it in every scan (a contact that
     * only flickered closed, or noise, doesn't press); any scan that saw it
     * closed keeps it held for BTN_STABLE more samples. */
#if GM_BTN_GESTURES
    uint32_t now_ms = hal_millis();
#endif
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
#if GM_MODE_ONE_SHOT
            if (down)
                mode_one_shot_press(i);
#endif
#if GM_BTN_DOUBLE || GM_N_COMBOS
            if (down)
                gestures_before_press(i, now_ms);
#endif
            if (down)
                btn_held |= 1ull << i;
            else
                btn_held &= ~(1ull << i);
            if ((btn_ignore >> i) & 1u) {
                btn_ignore &= ~(1ull << i);   /* released after a preset switch */
#if GM_BTN_GESTURES
                gesture_forget(i);
#endif
#if GM_N_MODES > 0
                mode_late_release(i);
#endif
                continue;
            }
#if GM_N_MODES > 0
            if (mode_toggle_button(i, down))
                continue;
            if (mode_function_button(i, down))
                continue;
#endif
#if GM_BTN_GESTURES
            if (gesture_input(i, down, now_ms))
                continue;
#endif
            button_changed(i, down);
        }
    }
#if GM_BTN_GESTURES
    gestures_ms(now_ms);
#endif
}

/* The buttons set to cc_ramp, worked out again when a param changes: the
 * millisecond loop looks at those only. */
static btn_bits_t ramp_mask;
static uint32_t ramp_gen;

/* Called once per elapsed millisecond. */
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
#if GM_BTN_GESTURES
    btn_bits_t own_up = gestures_off();     /* first: a gesture's note is let go once */
#else
    const btn_bits_t own_up = 0;
#endif
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
        int held = ((btn_held & ~(uint64_t)own_up) >> i) & 1u;   /* with its own action down */
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
