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

#define FW_VERSION GEMIFI_VERSION(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH)

static uint8_t dev_desc[] = { USB_MIDI_DEVICE_DESC(0x1209, 0x0002, 0x0100) };
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC };
static char serial[25];
static const char *const strings[] = { "gemifi", GM_DEVICE_NAME " (gemifi)", serial };
static const usbd_desc_t usb_desc = {
    dev_desc, cfg_desc, sizeof(cfg_desc), 3, strings,
};

void TMR0_IRQHandler(void)
{
    TMR0_TISR = TISR_TIF;
    gm_scan_tick();
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
    MAILBOX->reason = 0;

    sys_clock_init();
    sys_tick_init();
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

    int cleared = 0;
    for (;;) {
        usbd_poll();
        midi_rx_drain(gm_midi_rx_byte);
        gm_poll();
        /* Boot-loop guard (bootloader-design.md 3.6): we ran for 5 s. */
        if (!cleared && sys_tick_ms() >= 5000u) {
            cleared = 1;
            MAILBOX->boot_count = 0;
        }
    }
}
