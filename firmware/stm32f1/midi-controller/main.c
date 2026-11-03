/*
 * midi-controller (STM32F1): the generic gemifi MIDI controller firmware.
 *
 * The hardware comes from a device description (devices/<device>.yaml ->
 * gm_device_config.h/.c, see core/README.md); the behaviour from the core
 * runtime (core/src/controller.c) and its config profile, editable over
 * SysEx from the web updater or tools/host/gemifi.py.
 *
 * This file is only the STM32F1 entry point: clocks, IWDG, USB-MIDI, the
 * scan timer (TIM2 interrupt at gm_device.scan_us) and the main loop.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "stm32f1.h"
#include "sys.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "app_proto.h"
#include "gm_feedback.h"

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

/* TIM2 counts microseconds (prescaled to 1 MHz). Longest scan seen, in us,
 * and whether the period had to be raised because a scan ran into the next
 * tick (which would starve the main loop, and with it USB). */
#define SCAN_TIM TIM2_BASE
static volatile uint32_t scan_max;
static volatile uint8_t scan_slowed;
#define SCAN_MAX_US 4000u

void TIM2_IRQHandler(void)
{
    TIM_SR(SCAN_TIM) = ~TIM_SR_UIF;     /* rc_w0 */
    gm_scan_tick();
    uint32_t used = TIM_CNT(SCAN_TIM);  /* counts up from 0 at each tick */
    if (used > scan_max)
        scan_max = used;
    if (TIM_SR(SCAN_TIM) & TIM_SR_UIF) {   /* the next tick came during this scan */
        uint32_t p = TIM_ARR(SCAN_TIM) + 1u;
        if (p < SCAN_MAX_US) {
            p += p / 4u;
            TIM_ARR(SCAN_TIM) = p - 1u;
            gm_scan_period_us = p;
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
    uint32_t period = TIM_ARR(SCAN_TIM) + 1u;
    app_log(LOG_INFO, fmt_num(a, "scan load ", scan_max * 100u / period, "%"),
            fmt_num(b, ", period ", period, " us"));
    if (scan_slowed)
        app_log(LOG_WARN, "scan overran its period: slowed down to ",
                fmt_num(b, "", period, " us"));
    if (!sys_hse_ok())
        app_log(LOG_WARN, "HSE crystal did not start: running on HSI, USB timing ",
                "out of spec");
}

static void scan_timer_start(uint32_t period_us)
{
    if (period_us < 50u)
        period_us = 50u;
    if (period_us > 65536u)
        period_us = 65536u;
    /* APB1 = HCLK / 2, so the TIM2 clock is 2 x PCLK1 = HCLK (RM0008 7.2). */
    RCC_APB1ENR |= RCC_APB1_TIM2;
    RCC_APB1RSTR |= RCC_APB1_TIM2;
    RCC_APB1RSTR &= ~RCC_APB1_TIM2;
    TIM_PSC(SCAN_TIM) = sys_hclk_hz / 1000000u - 1u;   /* 1 tick = 1 us */
    TIM_ARR(SCAN_TIM) = period_us - 1u;
    TIM_CR1(SCAN_TIM) = TIM_CR1_URS;
    TIM_EGR(SCAN_TIM) = TIM_EGR_UG;     /* load PSC now (URS: no UIF) */
    TIM_SR(SCAN_TIM) = 0;
    TIM_DIER(SCAN_TIM) = TIM_DIER_UIE;
    TIM_CR1(SCAN_TIM) = TIM_CR1_URS | TIM_CR1_CEN;
    NVIC_ISER(IRQ_TIM2 / 32) = 1u << (IRQ_TIM2 % 32);
    irq_enable_all();
}

int main(void)
{
    sys_iwdg_start();
    sys_clock_init();
    sys_tick_init();
    hal_delay_calibrate();
    (void)sys_reset_cause();               /* snapshot + clear the flags */

    /* Boot combo held at power-up: ignore the stored profile. (The
     * recovery combo, src/recovery.c, already ran in Reset_Handler.) */
    uint32_t combo = gm_device.boot_combo;
    int safe = combo && (gm_buttons_now() & combo) == combo;

    sys_iwdg_kick();
    gm_init(FW_VERSION, safe);
    sys_iwdg_kick();

    dev_desc[8] = gm_device.usb_vid & 0xFF;
    dev_desc[9] = gm_device.usb_vid >> 8;
    dev_desc[10] = gm_device.usb_pid & 0xFF;
    dev_desc[11] = gm_device.usb_pid >> 8;
    app_proto_uid_hex(serial);
    usbd_init(&usb_desc);

    scan_timer_start(gm_device.scan_us);

    /* No boot-loop guard here (the NUC's SRAM mailbox): on this board the
     * way back is the recovery combo at reset (vendor update-request flag)
     * and the IWDG, which turns a hang into a reset. */
    int reported = 0;
    for (;;) {
        sys_iwdg_kick();
        usbd_poll();
        midi_rx_drain(gm_midi_rx_usb);
        gm_poll();
        if (!reported && sys_tick_ms() >= 2500u) {
            reported = 1;
            report_scan();
        }
    }
}
