/*
 * midi-controller (NUC1xx): the generic gemifi MIDI controller firmware.
 *
 * The hardware comes from a device description (devices/<device>.yaml ->
 * gm_device_config.h/.c, see core/README.md); the behaviour from the core
 * runtime (core/src/controller.c) and its config profile, editable over
 * SysEx from the web updater or tools/host/gemifi.py.
 *
 * This file is only the NUC1xx entry point: clocks, USB-MIDI, the scan
 * timer (Timer0 interrupt at gm_device.scan_us) and the main loop.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "nuc1xx.h"
#include "sys.h"
#include "fmc.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "app_proto.h"
#include "gm_feedback.h"
#include "gm_safety.h"

#define FW_VERSION GEMIFI_VERSION(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH)

static uint8_t dev_desc[] = { USB_MIDI_DEVICE_DESC(0x1209, 0x0002, 0x0100) };
static char serial[25];
#if GM_USB_DAW_PORT
/* Two ports (cables) on the one endpoint pair; the jacks are named after
 * strings 4 and 5, which is what most OSes show as the port names. */
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC2(4, 5) };
static const char *const strings[] = {
    "gemifi", "gemifi (" GM_DEVICE_NAME ")", serial, GM_DEVICE_NAME, GM_DAW_PORT_NAME,
};
#else
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC };
static const char *const strings[] = { "gemifi", "gemifi (" GM_DEVICE_NAME ")", serial };
#endif
static const usbd_desc_t usb_desc = {
    dev_desc, cfg_desc, sizeof(cfg_desc), sizeof(strings) / sizeof(strings[0]), strings,
};

/* Longest scan seen, in timer ticks (12 per us), and whether the period had
 * to be raised because a scan ran into the next tick (which would starve
 * the main loop, and with it USB). */
static volatile uint32_t scan_max;
static volatile uint8_t scan_slowed;
static volatile uint32_t scan_ticks;    /* timer periods since the start: the main loop's clock */
#define SCAN_MAX_US 4000u

void TMR0_IRQHandler(void)
{
    TMR0_TISR = TISR_TIF;
    scan_ticks++;
    gm_scan_tick();
    uint32_t used = TMR0_TDR;           /* counts up from 0 at each tick */
    if (used > scan_max)
        scan_max = used;
    if (TMR0_TISR & TISR_TIF) {         /* the next tick came during this scan */
        uint32_t p = TMR0_TCMPR;
        if (p < 12u * SCAN_MAX_US) {
            p += p / 4u;
            TMR0_TCMPR = p;
            gm_scan_period_us = p / 12u;
            scan_slowed = 1;
        }
    }
}

/* "<text><n><unit>" into buf, for LOG messages (no printf here). */
static const char *fmt_num(char *buf, const char *text, uint32_t n, const char *unit)
{
    char *d = buf, digits[10];
    int k = 0;
    while (*text)
        *d++ = *text++;
    do {
        digits[k++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n && k < 10);
    while (k)
        *d++ = digits[--k];
    while (*unit)
        *d++ = *unit++;
    *d = 0;
    return buf;
}

static void report_scan(void)
{
    char a[32], b[32];
    uint32_t period = TMR0_TCMPR;
    app_log(LOG_INFO, fmt_num(a, "scan load ", scan_max * 100u / period, "%"),
            fmt_num(b, ", period ", period / 12u, " us"));
    if (scan_slowed)
        app_log(LOG_WARN, "scan overran its period: slowed down to ",
                fmt_num(b, "", period / 12u, " us"));
}

/* Timer counts (12 per us) since the start, from scan_ticks and the count
 * within the period: wall time, the scan interrupt included. */
static uint32_t timer_now(void)
{
    uint32_t t, c;
    do {
        t = scan_ticks;
        c = TMR0_TDR;
    } while (t != scan_ticks);
    return t * TMR0_TCMPR + c;
}

static void scan_timer_start(uint32_t period_us)
{
    if (period_us < 50u)
        period_us = 50u;
    CLK_CLKSEL1 &= ~CLKSEL1_TMR0_S_MASK;   /* 12 MHz crystal */
    CLK_APBCLK |= APBCLK_TMR0_EN;
    TMR0_TCSR = TCSR_CRST;
    TMR0_TCMPR = 12u * period_us;          /* 12 ticks per us */
    TMR0_TISR = TISR_TIF;
    TMR0_TCSR = TCSR_CEN | TCSR_IE | TCSR_MODE_PERIODIC | TCSR_PRESCALE(0);
    NVIC_ISER = 1u << IRQ_TMR0;
    irq_enable_all();
}

int main(void)
{
    hal_boot_capture();                    /* diag.reset_reason, diag.boot_loop */
    MAILBOX->reason = 0;

    sys_clock_init();
    sys_tick_init();
    hal_delay_calibrate();
    fmc_open();                            /* UID, profile in data flash */

    /* Boot combo held at power-up: ignore the stored profile. (With the
     * gemifi bootloader in LDROM the same combo keeps the bootloader in
     * update mode, so this only happens under the stock LDROM.) */
    uint32_t combo = gm_device.boot_combo;
    int safe = combo && (gm_buttons_now() & combo) == combo;

    gm_init(FW_VERSION, safe);

    dev_desc[8] = gm_device.usb_vid & 0xFF;
    dev_desc[9] = gm_device.usb_vid >> 8;
    dev_desc[10] = gm_device.usb_pid & 0xFF;
    dev_desc[11] = gm_device.usb_pid >> 8;
    app_proto_uid_hex(serial);
    usbd_init(&usb_desc);

    scan_timer_start(gm_device.scan_us);
    hal_midi_usb_rx(gm_midi_rx_usb);      /* OUT drained at each hal_midi_poll too */

    int cleared = 0, reported = 0;
    uint32_t seen_scan = 0, loop_max = 0, t_last = timer_now();
    for (;;) {
        usbd_poll();
        midi_rx_drain(gm_midi_rx_usb);
        gm_poll();
        /* diag.scan_max, diag.loop_max (gm_safety.h): converted only when
         * they grow (no divider on the M0). */
        uint32_t t_now = timer_now(), dt = t_now - t_last;
        t_last = t_now;
        if (dt > loop_max) {
            loop_max = dt;
            gm_diag_loop_us(dt / 12u);
        }
        if (scan_max != seen_scan) {
            seen_scan = scan_max;
            gm_diag_scan_us(seen_scan / 12u);
        }
        /* Boot-loop guard (bootloader-design.md 3.6): we ran for 5 s. */
        if (!reported && sys_tick_ms() >= 2500u) {
            reported = 1;
            report_scan();
            t_last = timer_now();          /* the report's own sending isn't a pass */
        }
        if (!cleared && sys_tick_ms() >= 5000u) {
            cleared = 1;
            MAILBOX->boot_count = 0;
        }
    }
}
