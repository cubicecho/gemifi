/*
 * safety.c: the factory-reset combo, physical unlock, the status LED's
 * blink codes and the diagnostics (gm_safety.h; core/README.md
 * "Factory-reset combo", "Physical unlock", "Status LED"; protocol/sysex.md
 * 4.9, 4.10).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_safety.h"
#include "gm_hal.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "gm_controller.h"
#include "gm_presets.h"
#include "gm_display.h"
#include "app_proto.h"

/* A config generated before these keys: all off. */
#ifndef GM_UNLOCK
#define GM_UNLOCK 0
#endif
#ifndef GM_RESET_COMBO
#define GM_RESET_COMBO 0
#endif
#ifndef GM_STATUS_LED
#define GM_STATUS_LED (-1)
#endif
#ifndef GM_N_BUTTONS
#define GM_N_BUTTONS 0
#endif

#define UNLOCK_WINDOW_MS 30000u      /* sysex.md 4.9 */
#define UNLOCK_IDLE_MS   300000u     /* unlocked: no request for 5 min -> locked */
#define RESET_WATCH_MS   1500u       /* the reset combo counts if held this soon after start */
#define CODE_SHOW_MS     10000u      /* error codes: this long after start */

/* Combo masks in the narrowest type that holds every button (64-bit
 * shifts are library calls on a Cortex-M0). */
#if GM_N_BUTTONS <= 32
typedef uint32_t mask_t;
#else
typedef uint64_t mask_t;
#endif

volatile uint32_t gm_diag_scan_max, gm_diag_loop_max;
static int32_t diag_reset, diag_loop, diag_settings;
static uint32_t t_start;

__attribute__((weak)) uint32_t hal_reset_reason(void)
{
    return GM_RESET_UNKNOWN;
}

__attribute__((weak)) int hal_boot_loop(void)
{
    return 0;
}

/* ---- the status LED's blink codes ---------------------------------------- */

#if GM_STATUS_LED >= 0
enum { PAT_NONE, PAT_CONFIRM, PAT_WIPED, PAT_RESET_HOLD, PAT_WINDOW };
static uint8_t pat_fixed;            /* a fixed pattern, or PAT_NONE (then the error code) */
static uint32_t pat_t0;
static uint8_t code;                 /* error code: flashes, 0 = none */
static int8_t level = -1;

static void pattern(uint8_t p, uint32_t now)
{
    pat_fixed = p;
    pat_t0 = now;
}

/* What the LED shows now: -1 nothing (its normal level), else 0 / 1. */
static int8_t status_at(uint32_t now)
{
    uint32_t t = now - pat_t0;
    switch (pat_fixed) {
    case PAT_WINDOW:                 /* unlock window: 4 Hz */
        return (int8_t)((t / 125u) & 1u ? 0 : 1);
    case PAT_RESET_HOLD:             /* reset combo held: 2 Hz */
        return (int8_t)((t / 250u) & 1u ? 0 : 1);
    case PAT_WIPED:                  /* settings wiped: 10 Hz for 1.5 s */
        if (t < 1500u)
            return (int8_t)((t / 50u) & 1u ? 0 : 1);
        break;
    case PAT_CONFIRM:                /* unlocked: on for 1 s */
        if (t < 1000u)
            return 1;
        break;
    default:
        break;
    }
    pat_fixed = PAT_NONE;
    if (code && now - t_start < CODE_SHOW_MS) {
        /* n flashes of 200 ms (300 ms apart), then 1.2 s dark, again */
        uint32_t c = (now - t_start) % (500u * code + 1200u);
        return (int8_t)(c < 500u * code && c % 500u < 200u);
    }
    return -1;
}

int gm_status_level(void)
{
    return level;
}

int gm_status_active(void)
{
    return level >= 0;
}
#else
#define pattern(p, now) ((void)(now))
int gm_status_level(void)
{
    return -1;
}

int gm_status_active(void)
{
    return 0;
}
#endif

/* ---- combo names --------------------------------------------------------- */

#if GM_UNLOCK || GM_RESET_COMBO
/* "<pre><button> + <button>..." into buf (n bytes), for logs and the display. */
static const char *combo_text(char *buf, uint32_t n, const char *pre, mask_t combo, const char *sep)
{
    uint32_t k = 0;
    for (const char *s = pre; *s && k + 1u < n; s++)
        buf[k++] = *s;
    int first = 1;
    for (uint32_t i = 0; i < gm_device.n_buttons && i < 8u * sizeof(mask_t); i++) {
        if (!((combo >> i) & 1u))
            continue;
        for (const char *s = first ? "" : sep; *s && k + 1u < n; s++)
            buf[k++] = *s;
        for (const char *s = gm_device.buttons[i].name; s && *s && k + 1u < n; s++)
            buf[k++] = *s;
        first = 0;
    }
    buf[k] = 0;
    return buf;
}
#endif

/* ---- physical unlock (sysex.md 4.9) -------------------------------------- */

#if GM_UNLOCK
#define UNLOCK_MASK ((mask_t)(GM_UNLOCK_COMBO))
static uint8_t unlocked, win_open, holding;
static uint32_t win_t0, hold_t0, idle_t0;

static void window_open(void)
{
    uint32_t now = hal_millis();
    if (!win_open) {
        char buf[48];
        app_log(LOG_INFO, "locked: to unlock, hold ", combo_text(buf, sizeof(buf), "", GM_UNLOCK_COMBO, " + "));
        gm_display_message(combo_text(buf, 25u, "HOLD ", GM_UNLOCK_COMBO, "+"));
    }
    win_open = 1;
    holding = 0;
    win_t0 = now;
    pattern(PAT_WINDOW, now);
}

int gm_unlock_check(void)
{
    if (unlocked)
        return 0;
    window_open();
    return 1;
}

void gm_unlock_activity(void)
{
    idle_t0 = hal_millis();
}

uint32_t gm_unlock_state(int open, uint32_t *window_s)
{
    if (open && !unlocked)
        window_open();
    uint32_t left = 0;
    if (win_open) {
        uint32_t t = hal_millis() - win_t0;
        left = t < UNLOCK_WINDOW_MS ? (UNLOCK_WINDOW_MS - t + 999u) / 1000u : 0;
    }
    *window_s = left;
    return unlocked;
}

static void unlock_poll(uint32_t now, mask_t held)
{
    if (win_open) {
        if ((held & UNLOCK_MASK) == UNLOCK_MASK) {
            if (!holding) {
                holding = 1;
                hold_t0 = now;
            }
            if (now - hold_t0 >= GM_UNLOCK_HOLD_MS) {
                win_open = 0;
                unlocked = 1;
                idle_t0 = now;
                pattern(PAT_CONFIRM, now);
                gm_display_message("UNLOCKED");
                app_log(LOG_INFO, "unlocked", "");
                app_unlocked(1);
                return;
            }
        } else {
            holding = 0;
        }
        if (now - win_t0 >= UNLOCK_WINDOW_MS) {
            win_open = 0;
            pattern(PAT_NONE, now);
            gm_display_message("");
            app_log(LOG_INFO, "unlock window closed", "");
            app_unlocked(0);
        }
    } else if (unlocked && now - idle_t0 >= UNLOCK_IDLE_MS) {
        unlocked = 0;
        app_log(LOG_INFO, "locked again (idle)", "");
        app_unlocked(0);
    }
}
#else
int gm_unlock_check(void)
{
    return 0;
}

void gm_unlock_activity(void)
{
}

uint32_t gm_unlock_state(int open, uint32_t *window_s)
{
    (void)open;
    *window_s = 0;
    return 1;
}
#endif

/* ---- the factory-reset combo --------------------------------------------- */

#if GM_RESET_COMBO
#define RESET_MASK ((mask_t)(GM_RESET_COMBO))
enum { RS_WATCH, RS_HOLD, RS_OFF };
static uint8_t rs;
static uint32_t rs_t0;

static void reset_poll(uint32_t now, mask_t held)
{
    int all = (held & RESET_MASK) == RESET_MASK;
    if (rs == RS_WATCH) {
        if (all) {
            char buf[48];
            rs = RS_HOLD;
            rs_t0 = now;
            pattern(PAT_RESET_HOLD, now);
            gm_display_message("RESET?");
            app_log(LOG_WARN, "factory-reset combo held: keep holding to wipe the settings: ",
                    combo_text(buf, sizeof(buf), "", GM_RESET_COMBO, " + "));
        } else if (now - t_start >= RESET_WATCH_MS) {
            rs = RS_OFF;
        }
    } else if (rs == RS_HOLD) {
        if (!all) {
            rs = RS_OFF;                 /* let go early: nothing happens */
            pattern(PAT_NONE, now);
            gm_display_message("");
            app_log(LOG_INFO, "factory reset cancelled", "");
        } else if (now - rs_t0 >= GM_RESET_HOLD_MS) {
            rs = RS_OFF;
            int st = presets_factory_reset();
            app_on_param_set(0, 0);
            diag_settings = GM_SETTINGS_WIPED;
            gm_diag_refresh();
            pattern(PAT_WIPED, now);
            gm_display_message(st == GSX_OK ? "RESET" : "ERROR");
            app_log(st == GSX_OK ? LOG_WARN : LOG_ERROR,
                    st == GSX_OK ? "settings wiped (factory-reset combo)" : "factory reset: flash error", "");
        }
    }
}
#endif

/* ---- the buttons ---------------------------------------------------------- */

#if GM_UNLOCK || GM_RESET_COMBO
static mask_t swallowed;             /* presses taken here: their releases are too */
#endif

int gm_safety_button(uint32_t i, int down)
{
#if GM_UNLOCK || GM_RESET_COMBO
    if (i >= GM_N_BUTTONS)
        return 0;
    mask_t bit = (mask_t)1 << i;
    if (!down) {
        if (!(swallowed & bit))
            return 0;
        swallowed &= ~bit;
        return 1;
    }
    mask_t take = 0;
#if GM_UNLOCK
    if (win_open)
        take |= UNLOCK_MASK;
#endif
#if GM_RESET_COMBO
    if (rs != RS_OFF)
        take |= RESET_MASK;
#endif
    if (!(take & bit))
        return 0;
    swallowed |= bit;
    return 1;
#else
    (void)i;
    (void)down;
    return 0;
#endif
}

/* ---- init and poll -------------------------------------------------------- */

void gm_safety_init(int settings)
{
    /* Everything from scratch (a test may start the core more than once). */
    t_start = hal_millis();
    gm_diag_scan_max = gm_diag_loop_max = 0;
#if GM_UNLOCK
    unlocked = win_open = holding = 0;
#endif
#if GM_RESET_COMBO
    rs = RS_WATCH;
#endif
#if GM_UNLOCK || GM_RESET_COMBO
    swallowed = 0;
#endif
#if GM_STATUS_LED >= 0
    pat_fixed = PAT_NONE;
    level = -1;
#endif
    diag_settings = settings;
    diag_reset = (int32_t)hal_reset_reason();
    diag_loop = hal_boot_loop() ? 1 : 0;
    gm_diag_refresh();
#if GM_STATUS_LED >= 0
    /* The error code to show (core/README.md "Status LED"): the most
     * serious one. */
    code = diag_loop ? 3u : diag_reset == GM_RESET_WATCHDOG || diag_reset == GM_RESET_CRASH ? 4u :
           settings == GM_SETTINGS_REFUSED ? 2u : 0u;
#endif
}

void gm_diag_refresh(void)
{
    gm_diag_param[0] = diag_reset;
    gm_diag_param[1] = diag_loop;
    gm_diag_param[2] = diag_settings;
    gm_diag_param[3] = (int32_t)gm_diag_scan_max;
    gm_diag_param[4] = (int32_t)gm_diag_loop_max;
    gm_diag_param[5] = (int32_t)gm_scan_period_us;
    gm_diag_param[6] = (int32_t)(hal_millis() / 1000u);
}

void gm_safety_poll(uint32_t now, uint64_t held)
{
#if GM_UNLOCK
    unlock_poll(now, (mask_t)held);
#endif
#if GM_RESET_COMBO
    reset_poll(now, (mask_t)held);
#endif
    (void)held;
#if GM_STATUS_LED >= 0
    level = status_at(now);
#endif
    gm_diag_refresh();
}
