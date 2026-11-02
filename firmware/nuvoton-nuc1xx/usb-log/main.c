/*
 * usb-log: milestone M2 (bootloader-design.md 10).
 *
 * Class-compliant USB-MIDI app, 1209:0002 "gemifi (Keystation Mini 32)".
 *   - Each button press/release: Note On/Off (channel 1, note 48 + button
 *     index), then an EVENT (type 1 = button) and a LOG SysEx message.
 *   - Answers HELLO (role 0) and the Universal Identity Request.
 *   - REBOOT(0/1): reply, wait ~50 ms, (mode 1: set the mailbox 'BOOT'
 *     magic), drive SE0 ~10 ms, then ISPCON.BS = 1 + CPU_RST, so the next
 *     code to run is the LDROM (sysex.md 6). With the stock LDROM still
 *     installed it hands straight back to us; with the gemifi bootloader,
 *     mode 1 stays in it.
 *   - FW_* commands get BAD_STATE (they belong to the bootloader).
 *   - About 1.5 s after the host configures the device it logs the reset
 *     source and the mailbox it found at boot (useful for M2/M4 testing).
 * Polled main loop, no interrupts.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "nuc1xx.h"
#include "sys.h"
#include "fmc.h"
#include "gsx.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"
#include "board.h"

#define FW_VERSION GEMIFI_VERSION(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH)
#define NOTE_BASE  48
#define REBOOT_DELAY_MS 50u

static const uint8_t dev_desc[] = { USB_MIDI_DEVICE_DESC(0x1209, 0x0002, 0x0100) };
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC };
static char serial[25];
static const char *const strings[] = { "gemifi", "gemifi (" BOARD_NAME ")", serial };
static const usbd_desc_t usb_desc = {
    dev_desc, cfg_desc, sizeof(cfg_desc), 3, strings,
};

static const char *const btn_names[8] = {
    "?", "sustain", "edit", "oct-", "oct+", "pitch-down", "pitch-up", "mod",
};

static uint8_t rx_buf[160];
static gsx_rx_t rx;
static uint8_t tx_buf[48];           /* last request reply, kept for resends */
static uint16_t tx_len;
static uint32_t last_req;            /* 0x10000 | seq << 8 | cmd, 0 = none */
static uint32_t uid[3];                 /* LE words = the HELLO uid bytes */
static uint8_t unsol_seq;
static uint8_t reboot_mode;          /* 0 none, 1 app, 2 bootloader */
static uint32_t reboot_at;
static uint32_t boot_rstsrc;
static gemifi_mailbox_t boot_mb;

static void make_serial(void)
{
    static const char hex[] = "0123456789ABCDEF";
    fmc_open();
    fmc_read_uid(uid);
    for (uint32_t i = 0; i < 24; i++)
        serial[i] = hex[(uid[i / 8] >> (28 - 4 * (i & 7))) & 0xF];
}

/* ---- unsolicited messages --------------------------------------------- */

static void send_log(uint8_t level, const char *a, const char *b)
{
    uint8_t buf[GSX_OVERHEAD + 2 + 1 + 4 + 64];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_LOG, unsol_seq++ & 0x7F);
    gsx_put(&t, level);
    gsx_put_u(&t, sys_tick_ms(), 4);
    uint32_t n = 0;
    for (const char *s = a; s && *s && n < 64; s++, n++)
        gsx_put(&t, (uint8_t)*s);
    for (const char *s = b; s && *s && n < 64; s++, n++)
        gsx_put(&t, (uint8_t)*s);
    uint16_t len = gsx_tx_end(&t);
    midi_tx_sysex(buf, len);
}

static void send_event(uint8_t type, uint32_t index, uint32_t value)
{
    uint8_t buf[GSX_OVERHEAD + 2 + 1 + 2 + 2 + 4];
    gsx_tx_t t;
    gsx_tx_begin(&t, buf, GSX_MSG_EVENT, unsol_seq++ & 0x7F);
    gsx_put(&t, type);
    gsx_put_u(&t, index, 2);
    gsx_put_u(&t, value, 2);
    gsx_put_u(&t, sys_tick_ms(), 4);
    uint16_t len = gsx_tx_end(&t);
    midi_tx_sysex(buf, len);
}

static char *put_hex(char *p, uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--)
        *p++ = hex[(v >> (4 * i)) & 0xF];
    return p;
}

static void log_boot_info(void)
{
    char s[64];
    char *p = s;
    static const char a[] = "boot rstsrc=";
    for (const char *q = a; *q; q++)
        *p++ = *q;
    p = put_hex(p, boot_rstsrc, 2);
    static const char b[] = " mb=";
    for (const char *q = b; *q; q++)
        *p++ = *q;
    p = put_hex(p, boot_mb.magic, 8);
    *p++ = ' ';
    p = put_hex(p, boot_mb.magic_inv, 8);
    *p++ = ' ';
    p = put_hex(p, boot_mb.boot_count, 8);
    *p++ = ' ';                             /* reason: 1 REBOOT(1), 2 boot loop */
    p = put_hex(p, boot_mb.reason, 8);
    *p = 0;
    send_log(2, s, 0);
    midi_tx_flush(50);
}

/* ---- requests --------------------------------------------------------- */

static void identity_reply(void)
{
    /* Universal Identity Reply: F0 7E <dev> 06 02 <mfr> <family u14>
     * <model u14> <version 4 bytes> F7. Manufacturer = our SysEx ID's
     * first byte (0x7D, sysex.md 1), family = 'g' 'm', model = board_id. */
    const uint8_t msg[] = {
        0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x7D, 0x67, 0x6D,
        BOARD_ID & 0x7F, (BOARD_ID >> 7) & 0x7F,
        FW_VERSION_MAJOR & 0x7F, FW_VERSION_MINOR & 0x7F,
        FW_VERSION_PATCH & 0x7F, 0x00, 0xF7,
    };
    midi_tx_sysex(msg, sizeof(msg));
    midi_tx_flush(200);
}

static uint32_t cmd_hello(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 0)
        return GSX_BAD_LEN;
    gsx_put(t, 0);                          /* role: app */
    gsx_put_u(t, BOARD_ID, 2);
    gsx_put_u(t, FW_VERSION, 5);
    gsx_put(t, GSX_PROTO_VERSION);
    gsx_put_u(t, 0, 2);                     /* max_chunk: none in the app */
    gsx_put_u(t, GSX_CAP_LOG | GSX_CAP_EVENT, 2);
    gsx_put_packed(t, (const uint8_t *)uid, sizeof(uid));
    return GSX_OK;
}

static void handle_frame(void)
{
    /* Universal Identity Request: F0 7E <dev> 06 01 F7 */
    if (rx.state == 1 && rx.n == 4 && rx_buf[0] == 0x7E && rx_buf[2] == 0x06 &&
        rx_buf[3] == 0x01) {
        identity_reply();
        return;
    }
    gsx_frame_t f;
    int pst = gsx_parse(&rx, &f);
    if (pst == GSX_NOT_OURS)
        return;
    if (pst != GSX_OK) {
        uint8_t err[GSX_OVERHEAD + 4];
        gsx_tx_t t;
        gsx_tx_begin(&t, err, f.cmd | GSX_REPLY, f.seq);
        gsx_put(&t, (uint8_t)pst);
        midi_tx_sysex(err, gsx_tx_end(&t));
        midi_tx_flush(200);
        return;
    }
    uint32_t req = 0x10000u | (uint32_t)f.seq << 8 | f.cmd;
    if (req != last_req) {
        gsx_tx_t t;
        gsx_tx_begin(&t, tx_buf, f.cmd | GSX_REPLY, f.seq);
        uint16_t status_at = t.n;
        gsx_put(&t, 0);
        uint32_t st;
        switch (f.cmd) {
        case GSX_CMD_HELLO:
            st = cmd_hello(&f, &t);
            break;
        case GSX_CMD_REBOOT:
            if (f.len != 1)
                st = GSX_BAD_LEN;
            else if (f.payload[0] > 1)
                st = GSX_BAD_CMD;
            else {
                st = GSX_OK;
                reboot_mode = f.payload[0] + 1;
                reboot_at = sys_tick_ms() + REBOOT_DELAY_MS;
            }
            break;
        case GSX_CMD_FW_BEGIN:
        case GSX_CMD_FW_DATA:
        case GSX_CMD_FW_END:
        case GSX_CMD_FW_ABORT:
            st = GSX_BAD_STATE;             /* reboot to the bootloader first */
            break;
        default:
            st = GSX_BAD_CMD;
            break;
        }
        if (st != GSX_OK)
            t.n = status_at + 1u;
        tx_buf[status_at] = (uint8_t)st;
        tx_len = gsx_tx_end(&t);
        last_req = req;
    }
    midi_tx_sysex(tx_buf, tx_len);
    midi_tx_flush(200);
}

__attribute__((noreturn)) static void do_reboot(void)
{
    if (reboot_mode == 2) {
        MAILBOX->magic = MAILBOX_MAGIC_BOOT;
        MAILBOX->magic_inv = ~MAILBOX_MAGIC_BOOT;
        MAILBOX->reason = 1;                /* 1 = REBOOT(bootloader) from app */
    }
    usbd_disconnect();
    sys_delay_ms(10);
    sys_reset_to(1);                        /* via LDROM, both modes */
}

/* ---- buttons ---------------------------------------------------------- */

static void button_changed(uint32_t idx, int down)
{
    uint8_t note = (uint8_t)(NOTE_BASE + idx);
    if (down)
        midi_tx_event(0x09, 0x90, note, 100);
    else
        midi_tx_event(0x08, 0x80, note, 0);
    send_event(1, idx, (uint32_t)down);
    send_log(2, btn_names[idx], down ? " down" : " up");
    midi_tx_flush(20);
}

int main(void)
{
    /* Snapshot what the previous stage left before anything changes it. */
    boot_rstsrc = GCR_RSTSRC;
    boot_mb.magic = MAILBOX->magic;
    boot_mb.magic_inv = MAILBOX->magic_inv;
    boot_mb.boot_count = MAILBOX->boot_count;
    boot_mb.reason = MAILBOX->reason;
    MAILBOX->reason = 0;

    sys_clock_init();
    board_gpio_init();
    sys_tick_init();
    make_serial();
    rx.buf = rx_buf;
    rx.cap = sizeof(rx_buf);
    usbd_init(&usb_desc);

    uint32_t stable = board_buttons(), last_sample = stable;
    uint32_t t_scan = sys_tick_ms(), t_led = t_scan, t_cfg = 0;
    int logged = 0, cleared = 0, led = 0;

    for (;;) {
        usbd_poll();
        uint32_t now = sys_tick_ms();

        if (midi_rx_poll(&rx)) {
            handle_frame();
            gsx_rx_reset(&rx);
        }

        /* Buttons: sample every 5 ms, accept a change after 2 equal samples. */
        if (now - t_scan >= 5) {
            t_scan = now;
            uint32_t s = board_buttons();
            if (s == last_sample && s != stable) {
                uint32_t diff = s ^ stable;
                stable = s;
                for (uint32_t i = BOARD_BTN_FIRST; i <= BOARD_BTN_LAST; i++)
                    if (diff & (1u << i))
                        button_changed(i, (s >> i) & 1u);
            }
            last_sample = s;
        }

        /* Oct- LED: 1 Hz heartbeat. Oct+ LED: on while USB is configured. */
        if (now - t_led >= 500) {
            t_led = now;
            led = !led;
            board_led(LED_OCT_DOWN, led);
        }
        board_led(LED_OCT_UP, usbd_configured());

        if (usbd_configured()) {
            if (!t_cfg)
                t_cfg = now | 1u;
            if (!logged && now - t_cfg >= 1500) {
                logged = 1;
                log_boot_info();
            }
        }
        /* Boot-loop guard (bootloader-design.md 3.6): we ran for 5 s. */
        if (!cleared && now >= 5000) {
            cleared = 1;
            MAILBOX->boot_count = 0;
        }

        if (reboot_mode && (int32_t)(now - reboot_at) >= 0)
            do_reboot();
    }
}
