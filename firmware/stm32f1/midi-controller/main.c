/*
 * midi-controller (STM32F1): the generic gemifi MIDI controller firmware.
 *
 * The hardware comes from a device description (devices/<device>.yaml ->
 * gm_device_config.h/.c, see core/README.md); the behaviour from the core
 * runtime (core/src/controller.c) and its config profile, editable over
 * SysEx from the web updater or tools/host/gemifi.py.
 *
 * This file is only the STM32F1 entry point: clocks, IWDG, USB-MIDI, the
 * scan timer (TIM2 interrupt at gm_device.scan_us, or TIM3 / TIM4 when a
 * hardware scan has TIM2: GM_SCAN_TICK_TIM, dma_map.h), the hardware scans
 * (hardware_scan: in the YAML: src/scan_dma.c for the matrix, src/adc_dma.c
 * for the analog inputs and pads) and the main loop.
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
#include "gm_din.h"
#include "scan_dma.h"
#include "adc_dma.h"
#include "dma_map.h"
#include "board.h"
#include "gm_presets.h"

#ifdef BOARD_POWER_LATCH_PIN
/* Soft power (board header): the power button held 2 s switches off, as the
 * stock app and the bootloader do. A press that powered the board on counts
 * only once it has been released. */
#define POWER_OFF_HOLD_MS 2000u
static uint32_t power_btn_since;    /* released at (| 1); 0 = not yet released */

static void power_off(void)
{
    presets_flush();                 /* a preset switch still waiting to be stored */
    usbd_disconnect();
    GPIO_BRR(PORTC) = 1u << BOARD_POWER_LATCH_PIN;   /* off once the button is released */
    /* The board runs on the button until then, and on its capacitors for a
     * while after. Nothing here may reset: any reset (ours, the watchdog's,
     * a fault spinning into it) runs the bootloader, which latches the
     * power on again, so the keyboard rebooted instead of switching off.
     * The button can't be trusted either (it reads pressed as the supply
     * falls). So: no interrupts, no button, just keep the watchdog quiet. */
    irq_disable_all();
    for (;;)
        sys_iwdg_kick();
}

static void power_poll(uint32_t now)
{
    int pressed = !(GPIO_IDR(PORTC) & (1u << BOARD_POWER_BTN_PIN));
    if (!pressed)
        power_btn_since = now | 1u;
    else if (power_btn_since && now - power_btn_since >= POWER_OFF_HOLD_MS)
        power_off();
}
#else
static void power_poll(uint32_t now) { (void)now; }
#endif

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
#elif GM_DIN && GM_DIN_MODE == 1
/* din_midi: separate_port: cable 1 is the DIN jacks (core/src/din.c). */
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC2(4, 5) };
static const char *const strings[] = {
    "gemifi", "gemifi (" GM_DEVICE_NAME ")", serial, GM_DEVICE_NAME, GM_DIN_PORT_NAME,
};
#else
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC };
static const char *const strings[] = { "gemifi", "gemifi (" GM_DEVICE_NAME ")", serial };
#endif
static const usbd_desc_t usb_desc = {
    dev_desc, cfg_desc, sizeof(cfg_desc), sizeof(strings) / sizeof(strings[0]), strings,
};

/* TIM2 (GM_SCAN_TICK_TIM) ticks the scan (prescaled to 1 MHz). Scan time is measured with the
 * DWT cycle counter from the handler's entry, so a late start (the main loop
 * stalled, e.g. by a flash erase) isn't counted: those count as "late". The
 * period is raised when a scan takes more than 80% of it, which would starve
 * the main loop and with it USB. */
#define SCAN_TIM TIM_BASE_N(GM_SCAN_TICK_TIM)
#define SCAN_IRQ IRQ_TIM(GM_SCAN_TICK_TIM)
_Static_assert(GM_SCAN_TICK_TIM >= 2 && GM_SCAN_TICK_TIM <= 4, "the scan tick: TIM2..TIM4");
_Static_assert(!GM_HW_SCAN || GM_SCAN_TICK_TIM != GM_HW_SCAN_TIM, "the scan tick has the matrix scan's timer");
_Static_assert(!GM_HW_ADC || GM_SCAN_TICK_TIM != GM_HW_ADC_TIM, "the scan tick has the analog scan's timer");
static volatile uint32_t scan_max, scan_sum, scan_n, scan_late;
/* Main loop passes, wall time in cycles (the scan interrupt included): the
 * slowest one, and how many took over 1 ms / 2 ms. USB is polled once per
 * pass, and the host allows ~2 ms after SET_ADDRESS. */
static uint32_t loop_max, loop_over1, loop_over2;
static volatile uint8_t scan_slowed;
/* hal.c: MIDI sends that waited over 100 us for the host, and the longest. */
extern volatile uint32_t hal_midi_waits, hal_midi_wait_max_us;
static uint8_t hw_scan_failed;          /* hardware_scan.matrix given, but scan_dma refused the plan */
static uint8_t hw_adc_failed;           /* hardware_scan.analog given, but adc_dma refused it */
static uint32_t hw_scan_rows;
#define SCAN_MAX_US 4000u

void TIM_IRQ_HANDLER(GM_SCAN_TICK_TIM)(void)
{
    TIM_SR(SCAN_TIM) = ~TIM_SR_UIF;     /* rc_w0 */
    uint32_t c0 = DWT_CYCCNT;           /* cycle counter: can't wrap within a scan */
    gm_scan_tick();
    uint32_t used = (DWT_CYCCNT - c0) / (sys_hclk_hz / 1000000u);
    if (used == 0 && (TIM_SR(SCAN_TIM) & TIM_SR_UIF))
        used = TIM_ARR(SCAN_TIM) + 1u;  /* no cycle counter (a clone?): an overrun is a full period */
    if (used > scan_max)
        scan_max = used;
    scan_sum += used;
    scan_n++;
    uint32_t p = TIM_ARR(SCAN_TIM) + 1u;
    if (5u * used > 4u * p) {
        /* The scan takes over 80% of the period: give it 1.5x what it took,
         * so the main loop (and USB) keeps a third of the time. */
        uint32_t np = used + used / 2u;
        if (np < p + p / 4u)
            np = p + p / 4u;
        if (np > SCAN_MAX_US)
            np = SCAN_MAX_US;
        if (np != p) {
            TIM_ARR(SCAN_TIM) = np - 1u;
            gm_scan_period_us = np;
            scan_slowed = 1;
        }
    } else if (TIM_SR(SCAN_TIM) & TIM_SR_UIF) {
        scan_late++;                    /* started late, the scan itself fits */
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
    uint32_t avg = scan_n ? scan_sum / scan_n : 0;
    app_log(LOG_INFO, fmt_num(a, "scan avg ", avg, " us"), fmt_num(b, ", max ", scan_max, " us"));
    app_log(LOG_INFO, fmt_num(a, "scan period ", period, " us"), fmt_num(b, ", late starts ", scan_late, ""));
    uint32_t mhz = sys_hclk_hz / 1000000u;
    app_log(LOG_INFO, fmt_num(a, "scan keys ", gm_scan_part_max[0] / mhz, " us"),
            fmt_num(b, ", panel ", gm_scan_part_max[1] / mhz, " us"));
    app_log(LOG_INFO, fmt_num(a, "scan encoders ", gm_scan_part_max[2] / mhz, " us"),
            fmt_num(b, ", clock ", mhz, " MHz"));
    app_log(LOG_INFO, fmt_num(a, "main loop max ", loop_max, " us"),
            fmt_num(b, ", over 1 ms ", loop_over1, ""));
    app_log(LOG_INFO, fmt_num(a, "main loop over 2 ms ", loop_over2, ""), "");
    /* gm_poll parts (scan interrupt included): proto, keys/buttons, analog, pads,
     * encoders, LEDs (gm_controller.h; keys/buttons run first) */
    app_log(LOG_INFO, fmt_num(a, "poll proto ", gm_poll_part_max[0] / mhz, " us"),
            fmt_num(b, ", keys/buttons ", gm_poll_part_max[1] / mhz, " us"));
    app_log(LOG_INFO, fmt_num(a, "poll analog ", gm_poll_part_max[2] / mhz, " us"),
            fmt_num(b, ", pads ", gm_poll_part_max[3] / mhz, " us"));
    app_log(LOG_INFO, fmt_num(a, "poll encoders ", gm_poll_part_max[4] / mhz, " us"),
            fmt_num(b, ", led update ", gm_poll_part_max[5] / mhz, " us"));
    app_log(LOG_INFO, fmt_num(a, "poll led writes ", gm_poll_part_max[6] / mhz, " us"),
            fmt_num(b, ", midi out waits ", hal_midi_waits, ""));
    app_log(LOG_INFO, fmt_num(a, "midi out longest wait ", hal_midi_wait_max_us, " us"), "");
#if GM_DIN
    {
        extern volatile uint32_t hal_din_overruns;
        app_log(LOG_INFO, fmt_num(a, "din out drops ", gm_din_out_drops, ""),
                fmt_num(b, ", in overruns ", hal_din_overruns, ""));
    }
#endif
    if (scan_slowed)
        app_log(LOG_WARN, "scan overran its period: slowed down to ",
                fmt_num(b, "", period, " us"));
    if (gm_scan_hw()) {
        /* The matrix is the DMA's: the scan tick above only has the rest. */
        app_log(LOG_INFO, fmt_num(a, "hw scan frame ", scan_dma_frame_ns(), " ns"),
                fmt_num(b, ", rows ", hw_scan_rows, ""));
        app_log(LOG_INFO, fmt_num(a, "hw scan irq max ", scan_dma_isr_max_us, " us"),
                fmt_num(b, ", overruns ", scan_dma_overruns, ""));
        if (scan_dma_errors)
            app_log(LOG_WARN, "hw scan: DMA transfer error, the matrix scan stopped", "");
    } else if (hw_scan_failed) {
        app_log(LOG_WARN, "hw scan: the plan can't run on this chip; scanning in software", "");
    }
    if (gm_adc_hw()) {
        app_log(LOG_INFO, fmt_num(a, "hw adc sweep ", adc_dma_sweep_us(), " us"),
                fmt_num(b, ", irq max ", adc_dma_isr_max_us, " us"));
        app_log(LOG_INFO, fmt_num(a, "hw adc overruns ", adc_dma_overruns, ""),
                fmt_num(b, ", resyncs ", adc_dma_resyncs, ""));
        if (adc_dma_errors)
            app_log(LOG_WARN, "hw adc: DMA transfer error, the analog sweeps stopped", "");
    } else if (hw_adc_failed) {
        app_log(LOG_WARN, "hw adc: the plan can't run on this chip; reading in software", "");
    }
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
    /* APB1 = HCLK / 2, so the TIM2..4 clock is 2 x PCLK1 = HCLK (RM0008 7.2). */
    tim_clock_reset(GM_SCAN_TICK_TIM);
    TIM_PSC(SCAN_TIM) = sys_hclk_hz / 1000000u - 1u;   /* 1 tick = 1 us */
    TIM_ARR(SCAN_TIM) = period_us - 1u;
    TIM_CR1(SCAN_TIM) = TIM_CR1_URS;
    TIM_EGR(SCAN_TIM) = TIM_EGR_UG;     /* load PSC now (URS: no UIF) */
    TIM_SR(SCAN_TIM) = 0;
    TIM_DIER(SCAN_TIM) = TIM_DIER_UIE;
    NVIC_IPR8(SCAN_IRQ) = 0x80;         /* below USB (0x40): USB may interrupt a scan */
    TIM_CR1(SCAN_TIM) = TIM_CR1_URS | TIM_CR1_CEN;
    NVIC_ISER(SCAN_IRQ / 32) = 1u << (SCAN_IRQ % 32);
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

    /* The hardware matrix scan, when the YAML asks for one: frames from the
     * DMA replace the scan tick's matrix work. Started while interrupts are
     * still off (scan_timer_start turns them on), so no frame comes before
     * gm_scan_hw_started(). */
    const gm_scan_plan_t *plan = gm_scan_plan();
    if (plan && scan_dma_start(plan)) {
        hw_scan_rows = plan->n_rows;
        gm_scan_hw_started(scan_dma_frame_q4());
    } else if (plan) {
        hw_scan_failed = 1;
    }
    const gm_adc_plan_t *aplan = gm_adc_plan();
    if (aplan && adc_dma_start(aplan))
        gm_adc_hw_started();
    else if (aplan)
        hw_adc_failed = 1;
    scan_timer_start(gm_device.scan_us);
    /* USB from its interrupt, above the scan: a long main-loop pass (the
     * pads, an LED refresh, a flash write) can't miss the host's deadlines
     * (2 ms after SET_ADDRESS). The millisecond count from SysTick's. */
    usbd_irq_enable(0x40);
    sys_tick_irq_enable();

    /* No boot-loop guard here (the NUC's SRAM mailbox): on this board the
     * way back is the recovery combo at reset (vendor update-request flag)
     * and the IWDG, which turns a hang into a reset. */
    /* Scan timing at 2.5 s (boot) and 10 s (steady state). */
    int reported = 0;
    uint32_t mhz = sys_hclk_hz / 1000000u, c_last = DWT_CYCCNT;
    for (;;) {
        sys_iwdg_kick();
        usbd_poll();
        midi_rx_drain(gm_midi_rx_usb);
        gm_poll();
        uint32_t c_now = DWT_CYCCNT, us = (c_now - c_last) / mhz;
        c_last = c_now;
        if (us > loop_max)
            loop_max = us;
        loop_over1 += us > 1000u;
        loop_over2 += us > 2000u;
        uint32_t now = sys_tick_ms();
        power_poll(now);
        if ((reported == 0 && now >= 2500u) || (reported == 1 && now >= 10000u)) {
            reported++;
            report_scan();
            /* The 10 s report then shows 2.5-10 s: steady state, not boot. */
            scan_max = scan_sum = scan_n = 0;
            loop_max = loop_over1 = loop_over2 = 0;
            for (uint32_t i = 0; i < 3u; i++)
                gm_scan_part_max[i] = 0;
            for (uint32_t i = 0; i < GM_POLL_PARTS; i++)
                gm_poll_part_max[i] = 0;
            scan_dma_isr_max_us = 0;        /* the hardware scans' too: steady state at 10 s */
            adc_dma_isr_max_us = 0;
            hal_midi_waits = hal_midi_wait_max_us = 0;
            c_last = DWT_CYCCNT;            /* the report's own sending isn't a pass */
        }
    }
}
