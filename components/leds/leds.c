/*
 * leds.c: the LEDs (leds:), a control component (README.md here): what
 * each LED shows (its button's or pad's state, a mode's, the host's), the
 * LED looks, who owns an LED, the TM1628 fade, the LED walk and the
 * encoder rings. The main loop (core/src/controller.c gm_poll) calls
 * leds_update every 10 ms and leds_apply on the passes between; host
 * feedback calls gm_led_host and the gm_led_script* functions. Always
 * built: the status LED and the lamp test use it on any device. Shares its
 * state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "gm_display.h"
#include "gm_safety.h"
#include "gm_vpin.h"
#include "gm_hal.h"

#if GM_HOST_FB
static uint32_t led_colour[GM_ARR(GM_N_LEDS)];  /* 0xRRGGBB, 8 bits a colour */
#endif
/* What each LED last showed: (level << 1) | blink phase, 0xFFFF = not known
 * (start, after the lamp test). leds_update() runs every 10 ms over every
 * LED; an LED whose level and phase didn't change isn't written again. */
static uint16_t led_shown[GM_ARR(GM_N_LEDS)];

/* Levels: 0 off, 1 on (pin), 2 both pins, 3 pin2, 4+ pin2 blinking
 * (bicolour). LV_BLINK | level: that level, blinking (host feedback).
 * RGB LEDs: LV_RGB | colour (bit 0 red, 1 green, 2 blue), from FB_LED_RGB;
 * any other level > 0 is white. Off (0) and a plain on (1; any on level of
 * a single-colour or RGB LED) show the LED's idle and active looks
 * (gm_led_t.looks); a blink's dark half is the idle look. */
/* Light the pins in mask m (bit 0 pin, 1 pin2, 2 pin3), the others dark. */
static void led_pins(const gm_led_t *l, uint32_t m)
{
    vpin_write(l->pin, m & 1u);
    if (!vpin_is_none(l->pin2))
        vpin_write(l->pin2, (m >> 1) & 1u);
    if (!vpin_is_none(l->pin3))
        vpin_write(l->pin3, (m >> 2) & 1u);
}

/* The LED's idle (active = 0) or active look, as a pin mask. */
static uint32_t led_look(const gm_led_t *l, int active)
{
    if (!(l->looks & 0x80u))
        return active ? (vpin_is_none(l->pin3) ? 1u : 7u) : 0u;
    return active ? (l->looks >> 3) & 7u : l->looks & 7u;
}

#if GM_HOST_FB
/* An RGB LED whose pins are one LED of a full-colour hub (VPIN_RGB: red,
 * green, blue in a row; the hub's rgb op checks that pin is an LED's first):
 * it can show any colour (vpin_rgb). */
static int led_full_colour(const gm_led_t *l)
{
    return (vpin_pin_caps(l->pin) & VPIN_RGB) &&
           l->pin2.hub == l->pin.hub && l->pin3.hub == l->pin.hub &&
           l->pin2.num == l->pin.num + 1u && l->pin3.num == l->pin.num + 2u;
}
#endif

static void led_show(const gm_led_t *l, uint32_t level, int blink_on)
{
#if GM_HOST_LEDS
    if (level & LV_BLINK)
        level = blink_on ? level & ~LV_BLINK : 0;
#endif
    uint32_t m;
    int look = -1;                  /* 0 idle, 1 active: m is that look */
    if (level == 0)
        m = led_look(l, look = 0);
    else if (level & LV_RGB) {
#if GM_HOST_FB
        if ((level & LV_COLOUR) && led_full_colour(l) &&
            vpin_rgb(l->pin, led_colour[(uint32_t)(l - gm_device.leds)]))
            return;
#endif
#if GM_MODE_RGB
        /* a step's full colour: the builder gives one only to an LED that
         * can show it (led_full_colour) */
        if ((level >> LV_STEP_SHIFT) && vpin_rgb(l->pin, gm_mode_rgb[(level >> LV_STEP_SHIFT) - 1u]))
            return;
#endif
        m = level & 7u;             /* these pins, whatever the kind */
    } else if (!vpin_is_none(l->pin3))
        m = led_look(l, look = 1);
    else if (vpin_is_none(l->pin2) || level == 1)
        m = led_look(l, look = 1);
    else                            /* bicolour 2 both, 3 pin2, 4+ pin2 blinking */
        m = level == 2 ? 3u : (level == 3 || blink_on) ? 2u : led_look(l, look = 0);
    if (look >= 0 && l->rgb[look] && vpin_rgb(l->pin, l->rgb[look]))
        return;                     /* a full-colour look (a full-colour hub's LEDs) */
    led_pins(l, m);
}

#if GM_HOST_LEDS
#define NL GM_ARR(GM_N_LEDS)
#define HOST_NONE 0xFFu
/* Level the host set (LV_* bits and 0..7), HOST_NONE = none, with two bits
 * no level uses: */
#define HOST_HOLD 0x08u             /* set by a DAW script: no timeout       */
#define HOST_BLACK 0x10u            /* ... its FB_LED_RGB black (leds_fade)  */
#define HOST_LEVEL(h) ((h) & ~(HOST_HOLD | HOST_BLACK))
static uint8_t led_host_lv[NL];
static uint16_t led_host_t[NL];     /* when: hal_millis() / 10 (the timeouts' unit) */

void gm_led_host(uint32_t led, uint32_t value)
{
    if (led >= gm_device.n_leds)
        return;
    led_host_lv[led] = (uint8_t)(value == 0 ? 0 : value == 1 ? (LV_BLINK | 1u) : 1u);
    led_host_t[led] = (uint16_t)(hal_millis() / 10u);
}

#if GM_HOST_FB
static int bicolour(uint32_t led)
{
    return !vpin_is_none(gm_device.leds[led].pin2) && vpin_is_none(gm_device.leds[led].pin3);
}

static void led_script_set(uint32_t led, uint32_t level)
{
    led_host_lv[led] = (uint8_t)(level | HOST_HOLD);
    led_host_t[led] = (uint16_t)(hal_millis() / 10u);
}

/* FB_LED levels (sysex.md 4.6.2): 0 off, 1 blink, 2..127 on; on a bicolour
 * LED 2..63 is colour 1 (level 1, its pin) and 64..127 colour 2 (level 3). */
void gm_led_script(uint32_t led, uint32_t level)
{
    if (led >= gm_device.n_leds)
        return;
    uint32_t lv = level == 0 ? 0 : level == 1 ? (LV_BLINK | 1u) : level >= 64 && bicolour(led) ? 3u : 1u;
    led_script_set(led, lv);
}

void gm_led_script_rgb(uint32_t led, uint32_t r, uint32_t g, uint32_t b, int blink)
{
    if (led >= gm_device.n_leds)
        return;
    uint32_t lv = 0, m = r > g ? r : g;
    if (b > m)
        m = b;
    if (!vpin_is_none(gm_device.leds[led].pin3)) {
        /* on/off per channel: lit if at least half the brightest one; an
         * LED that can show any colour gets the colour itself (0..127 per
         * channel to 0..255) */
        if (m) {
            lv = LV_RGB | (2u * r >= m) | (2u * g >= m) << 1 | (2u * b >= m) << 2;
            /* fade: chips dim with the colour too (leds_fade) */
            int full = led_full_colour(&gm_device.leds[led]);
            if (full || GM_TM_FADE) {
                uint32_t c = (r * 2u + (r >> 6)) << 16 | (g * 2u + (g >> 6)) << 8 | (b * 2u + (b >> 6));
                /* A new colour at the same level: draw it. Not on an on/off
                 * LED (fade: its pins don't change, and leds_fade must keep
                 * seeing it, or the chip jumps to full until it is drawn). */
                if (c != led_colour[led] && full)
                    led_shown[led] = 0xFFFFu;
                led_colour[led] = c;
                lv |= LV_COLOUR;
            }
        }
    } else if (m) {
        lv = bicolour(led) && g > r ? 3u : 1u;
    }
    /* black: a colour at brightness 0 */
    led_script_set(led, lv && blink ? (LV_BLINK | lv) : lv ? lv : HOST_BLACK);
}
#endif

void gm_host_release(uint32_t what)
{
    if (what == 0 || what == 3)
        gm_display_host_release();
    if (what == 0 || what == 1)
        for (uint32_t i = 0; i < gm_device.n_leds; i++)
            led_host_lv[i] = HOST_NONE;
#if GM_N_ANALOGS > 0
    if (what == 0 || what == 2)
        analog_pickup_off();
#endif
}

/* Who drives LED i (core/README.md "LED ownership"). local = the device's
 * own state for it (its button's or pad's), LOCAL_HAS set; an LED no button
 * or pad uses has no local state, so the host always drives it. now10 = hal_millis() / 10. */
#define LOCAL_HAS 0x80u
static uint32_t led_level(uint32_t i, uint32_t local, uint32_t now10)
{
    int has_local = (local & LOCAL_HAS) != 0;
    local &= ~LOCAL_HAS;
#if GM_LED_FB
    uint32_t owner = gm_led_fb[i].owner, timeout = gm_led_fb[i].timeout;
#else
    uint32_t owner = GM_LED_OWNER, timeout = GM_LED_TIMEOUT;
#endif
    uint32_t h = led_host_lv[i];
    if (owner == GM_OWNER_LOCAL)
        return local;
    if (owner == GM_OWNER_HOST || !has_local)
        return h == HOST_NONE ? 0 : HOST_LEVEL(h);
    if (h & HOST_HOLD)
        timeout = GM_TIMEOUT_NEVER;         /* a DAW script's state stays until released */
    if (h != HOST_NONE && timeout != GM_TIMEOUT_NEVER && (uint16_t)(now10 - led_host_t[i]) >= timeout)
        led_host_lv[i] = (uint8_t)(h = HOST_NONE);   /* host went quiet: local again */
    return h == HOST_NONE ? local : HOST_LEVEL(h);
}
#endif

/* Whether level shows differently in the two blink phases (led_show). */
static int led_blinks(const gm_led_t *l, uint32_t level)
{
#if GM_HOST_LEDS
    if (level & LV_BLINK)
        return 1;
#endif
    return !vpin_is_none(l->pin2) && vpin_is_none(l->pin3) && level >= 4u && !(level & LV_RGB);
}

#if GM_LAYERS
uint8_t led_dead[GM_ARR(GM_N_LEDS)];

void led_redraw(uint32_t i)
{
    led_shown[i] = 0xFFFFu;
}
#endif

static void led_set(uint32_t i, uint32_t level, int blink_on)
{
#if GM_LAYERS
    if (led_dead[i])
        return;                     /* its twin has the pins */
#endif
#if GM_STATUS_LED >= 0
    if (i == (uint32_t)GM_STATUS_LED && gm_status_level() >= 0) {
        level = (uint32_t)gm_status_level();    /* a blink code (gm_safety.h) */
        blink_on = 0;
    }
#endif
    /* The blink phase is only part of the key for a blinking level: a
     * steady LED isn't written again every 500 ms. */
    int ph = led_blinks(&gm_device.leds[i], level) && blink_on;
    uint16_t key = (uint16_t)(((level & 0x7FFFu) << 1) | (ph ? 1u : 0u));
    if (led_shown[i] == key)
        return;
    led_shown[i] = key;
    led_show(&gm_device.leds[i], level, blink_on);
}


#if GM_HOST_LEDS
/* leds_update() works out the buttons' and pads' own levels every 10 ms;
 * leds_apply() then shows LED_STEP LEDs per main-loop pass, so no one pass
 * carries all of them. */
#define LED_STEP 13u
static uint8_t local[NL];           /* level, | LOCAL_HAS: a button or pad has it */
static uint8_t led_next = 0xFFu;    /* next LED to show, 0xFF = none */
static uint32_t led_now, led_now10;
#endif

/* Diagnostic LED walk (GM_LED_WALK_MS, e.g. `make LED_WALK=500`): one LED
 * on at a time, each for GM_LED_WALK_MS, in leds: order, logging "walk LED
 * <n>". Holding any button pauses it. Normal LED behaviour is off. */
#ifndef GM_LED_WALK_MS
#define GM_LED_WALK_MS 0
#endif
#if GM_LED_WALK_MS
static void leds_walk(uint32_t now)
{
    static uint32_t step = 0xFFFFFFFFu, t_step;
    int paused = 0;
#if GM_N_BUTTONS > 0
    paused = btn_stable != 0;
#endif
    if (gm_device.n_leds == 0)
        return;
    if (step == 0xFFFFFFFFu || (!paused && now - t_step >= GM_LED_WALK_MS)) {
        step = step + 1u >= gm_device.n_leds ? 0 : step + 1u;
        t_step = now;
        char msg[16] = "LED ";
        char d[10];
        uint32_t n = step, k = 0, j = 4;
        do { d[k++] = (char)('0' + n % 10u); n /= 10u; } while (n);
        while (k)
            msg[j++] = d[--k];
        msg[j] = 0;
        app_log(LOG_INFO, "walk ", msg);
    }
    if (paused)
        t_step = now;
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        led_pins(&gm_device.leds[i], i == step ? 7u : 0u);   /* every pin, whatever its looks */
}
#endif

#if GM_TM_FADE && GM_HOST_FB
/* tm1628 fade: (vpin_fade): how bright each LED shows, from led_shown: a
 * DAW's colour at its brightest channel (these win), any other lit level or
 * look full. Run when leds_apply() has shown a whole round, and the TM1628
 * bytes wait for that (gm_poll), so a chip's new brightness goes out with
 * the round's bytes (tm_flush sends the brightness first), not a round
 * later: a dim host colour never shows at the old, full brightness. */
#define LEDS_FADE 1
/* LEDs on a fade chip (bit = LED), leds_init: the others aren't looked at. */
static uint32_t fade_leds[(GM_ARR(GM_N_LEDS) + 31u) / 32u];

static void leds_fade(void)
{
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        const gm_led_t *l = &gm_device.leds[i];
        uint32_t key = led_shown[i], level = key >> 1;
        if (!((fade_leds[i >> 5] >> (i & 31u)) & 1u) || key == 0xFFFFu)
            continue;
#if GM_HOST_LEDS
        /* A blink's dark half shows the idle look, but a host colour's keeps
         * its brightness: else the whole chip pumps with the blink. */
        if ((level & LV_BLINK) && !(key & 1u) && !(level & LV_COLOUR))
            level = 0;
        level &= ~LV_BLINK;
#endif
        uint32_t v = 255u, colour = (level & LV_COLOUR) != 0;
        if (colour) {
            uint32_t c = led_colour[i], r = c >> 16, g = (c >> 8) & 0xFFu, b = c & 0xFFu;
            v = r > g ? r : g;
            if (b > v)
                v = b;
        } else if (level == 0 && led_host_lv[i] == (HOST_HOLD | HOST_BLACK)) {
            colour = 1;             /* a host's black: brightness 0, not its idle look */
            v = 0;
        } else if (level == 0 && !led_look(l, 0) && !l->rgb[0])
            continue;               /* dark */
        vpin_fade(l->pin, v, (int)colour);
    }
    vpin_fade_apply();
}
#else
#define leds_fade() ((void)0)
#define LEDS_FADE 0
#endif

void leds_update(uint32_t now)
{
#if GM_LED_WALK_MS
    leds_walk(now);
    return;
#endif
    int blink_on = (now / 500u) & 1u;
#if GM_STATUS_LED >= 0
    /* A blink code on the status LED (gm_safety.h): drawn every update;
     * once it ends, the LED's own level is drawn again. */
    static uint8_t status_was;
    if (GM_STATUS_LED < (int)gm_device.n_leds) {
        if (gm_status_active())
            led_set(GM_STATUS_LED, 0, blink_on);
        else if (status_was)
            led_shown[GM_STATUS_LED] = 0xFFFFu;
        status_was = (uint8_t)gm_status_active();
    }
#endif
#if GM_HOST_LEDS
    for (uint32_t i = 0; i < NL; i++)
        local[i] = 0;
#endif
    for (uint32_t i = 0; i < gm_device.n_buttons; i++) {
        int8_t li = gm_device.buttons[i].led;
        if (li < 0 || (uint32_t)li >= gm_device.n_leds)
            continue;
        uint32_t level = 0;
#if GM_N_BUTTONS > 0
        int held = (btn_held >> i) & 1u;
        switch (gm_cfg.btn_action[i]) {
        case ACT_OCTAVE_DOWN:
            level = gm_cfg.octave < 0 ? (uint32_t)-gm_cfg.octave : 0;
            break;
        case ACT_OCTAVE_UP:
            level = gm_cfg.octave > 0 ? (uint32_t)gm_cfg.octave : 0;
            break;
        case ACT_TRANSPOSE_DOWN:
            level = gm_cfg.transpose < 0;
            break;
        case ACT_TRANSPOSE_UP:
            level = gm_cfg.transpose > 0;
            break;
        case ACT_CC_TOGGLE:
            level = (btn_toggled >> i) & 1u;
            break;
        case ACT_CC_RAMP:
            level = ramp_q8[i] > 0;
            break;
        case ACT_NONE:
            break;
        default:
            level = (uint32_t)held;
            break;
        }
#endif
#if GM_HOST_LEDS
        local[li] = (uint8_t)(level | LOCAL_HAS);
#else
        led_set((uint32_t)li, MODE_LED((uint32_t)li, level, now), blink_on);
#endif
    }
#if GM_N_PADS > 0
    /* A pad's LED is lit while its own hit is held; a twin's LED while the
     * twin's is (the base pad's state stays on the base LED). */
    for (uint32_t k = 0; k < NP + GM_PAD_TWINS; k++) {
        uint32_t i = k, owner = 0;
        int8_t li = gm_pads[k < NP ? k : 0].led;
#if GM_PAD_TWINS
        if (k >= NP) {
            i = gm_pad_twins[k - NP].base;
            li = gm_pad_twins[k - NP].led;
            owner = k - NP + 1u;
        }
#endif
        if (li < 0 || (uint32_t)li >= gm_device.n_leds)
            continue;
        uint32_t level = PAD_OWNER(i) == owner && (pad_state[i] == PAD_ON ||
                         (pad_lit[i] && now - pad_lit[i] < PAD_LED_MIN_MS));
#if GM_HOST_LEDS
        local[li] = (uint8_t)(level | LOCAL_HAS);
#else
        led_set((uint32_t)li, MODE_LED((uint32_t)li, level, now), blink_on);
#endif
    }
#endif
#if GM_N_MODES > 0 && !GM_HOST_LEDS
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        uint32_t lv = mode_led(i, 0xFFu, now);    /* also LEDs no button drives */
        if (lv != 0xFFu)
            led_set(i, lv, blink_on);
    }
#endif
#if !GM_HOST_LEDS
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        if (led_shown[i] == 0xFFFFu)
            led_set(i, 0, blink_on);    /* driven by nothing: its idle look, once */
#endif
#if GM_HOST_LEDS
    (void)blink_on;                 /* leds_apply() works it out */
    led_now = now;
    led_now10 = now / 10u;
    led_next = 0;
#endif
}

/* Show the next LED_STEP LEDs of the round leds_update() started; 1 if it
 * did any. */
int leds_apply(void)
{
#if GM_HOST_LEDS
    if (led_next >= gm_device.n_leds)
        return 0;
    uint32_t now = led_now, end = led_next + LED_STEP;
    int blink_on = (now / 500u) & 1u;
    for (uint32_t i = led_next; i < end && i < gm_device.n_leds; i++)
        led_set(i, MODE_LED(i, led_level(i, local[i], led_now10), now), blink_on);
    led_next = end >= gm_device.n_leds ? 0xFFu : (uint8_t)end;
    if (led_next == 0xFFu)
        leds_fade();                /* the round is shown: its brightness */
    return 1;
#else
    return 0;
#endif
}

int leds_round_open(void)
{
#if LEDS_FADE
    return led_next != 0xFFu;
#else
    return 0;
#endif
}

void leds_init(void)
{
#if GM_HOST_LEDS
    for (uint32_t i = 0; i < NL; i++)
        led_host_lv[i] = HOST_NONE;
#endif
#if GM_TM_FADE && GM_HOST_FB
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        if (vpin_fades(gm_device.leds[i].pin))
            fade_leds[i >> 5] |= 1u << (i & 31u);
        else
            fade_leds[i >> 5] &= ~(1u << (i & 31u));
#endif
}

void leds_lamp(int on)
{
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        led_pins(&gm_device.leds[i], on ? 7u : 0u);
        if (!on)
            led_shown[i] = 0xFFFFu;     /* leds_update writes each once (its idle look, if off) */
    }
}

/* ---- encoder rings (MCU V-Pots, DAW scripts) --------------------------- */

#if GM_RINGS && GM_N_ENCODERS > 0
void gm_ring_draw(uint32_t e, uint32_t mode, int32_t frac, int center)
{
    const gm_ring_t *r = &gm_encoder_ring[e];
    int32_t n = r->n;
    if (r->center != GM_RING_NO_CENTER)
        gm_led_host(r->center, center ? 127 : 0);
    if (!n)
        return;
    if (frac > 16383)
        frac = 16383;
    int32_t p = frac < 0 ? -1 : (frac * (n - 1) + 8191) / 16383;   /* LED index of the position */
    int32_t c = (n - 1) / 2;
    for (int32_t k = 0; k < n; k++) {
        int on = 0;
        if (frac >= 0) {
            switch (mode & 3u) {
            case 0:
                on = k == p;
                break;
            case 1:
                on = (k >= c && k <= p) || (k <= c && k >= p);
                break;
            case 2:
                on = k <= p;
                break;
            default: {
                int32_t w = frac * (n / 2) / 16383;                /* width either side */
                on = k >= c - w && k <= c + w;
                break;
            }
            }
        }
        gm_led_host(r->leds[k], on ? 127 : 0);
    }
}
#else
void gm_ring_draw(uint32_t e, uint32_t mode, int32_t frac, int center)
{
    (void)e;
    (void)mode;
    (void)frac;
    (void)center;
}
#endif

