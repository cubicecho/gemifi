/*
 * probe (STM32F1): the first non-stock image for the M-Audio Code 61
 * (research/devices/m-audio-code61/analysis/custom-firmware-path.md 8,
 * step 6). It proves the app contract (VTOR, clocks, IWDG, USB-MIDI at
 * 0x08002400 behind the vendor bootloader) and reports what we need to know
 * about the chip, as gemifi LOG messages that tools/host/gemifi.py shows:
 *
 *   - DBGMCU_IDCODE (DEV_ID, REV_ID; reads 0 on genuine ST F103s without a
 *     debugger, ST errata), SCB CPUID (Cortex-M3 r1p1 on ST, r2p1 on GD32),
 *   - flash size register, 96-bit UID,
 *   - FLASH_OBR (RDP bit) and FLASH_WRPR,
 *   - RCC_CSR reset flags, the RCC state the bootloader left, HSE start,
 *   - SRAM size (fault-safe probe, PROBE_RAM=1, the default),
 *   - the update-request flag and the vendor checksum halfword.
 * The report repeats every 2 s so a host that opens the port late sees it.
 *
 * Protocol: HELLO (role 0 = app, board 2, no upload cap), the Universal
 * Identity Request, REBOOT(0) = reset, REBOOT(1) = vendor update request
 * (flag page) + reset. FW_* get BAD_STATE (uploads go through the vendor
 * protocol). Self-contained on gsx.c (no params/config).
 *
 * PROBE_DUMP_BOOTLOADER=1 (make PROBE_DUMP_BOOTLOADER=1; off by default,
 * private use only, never commit the output): after the first report,
 * streams 0x08000000-0x080023FF as hex LOG lines "dump AAAAAAAA: <hex>".
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "stm32f1.h"
#include "sys.h"
#include "flash.h"
#include "recovery.h"
#include "gsx.h"
#include "gm_hal.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"
#include "board.h"

#ifndef PROBE_RAM
#define PROBE_RAM 1
#endif
#ifndef PROBE_DUMP_BOOTLOADER
#define PROBE_DUMP_BOOTLOADER 0
#endif

#define FW_VERSION GEMIFI_VERSION(FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH)
#define REBOOT_DELAY_MS 50u
#define REPORT_EVERY_MS 2000u

static const uint8_t dev_desc[] = { USB_MIDI_DEVICE_DESC(0x1209, 0x0002, 0x0100) };
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC };
static char serial[25];
static const char *const strings[] = { "gemifi", "gemifi (" BOARD_NAME " probe)", serial };
static const usbd_desc_t usb_desc = {
    dev_desc, cfg_desc, sizeof(cfg_desc), 3, strings,
};

static uint8_t rx_buf[160];
static gsx_rx_t rx;
static uint8_t tx_buf[48];           /* last request reply, kept for resends */
static uint16_t tx_len;
static uint32_t last_req;            /* 0x10000 | seq << 8 | cmd, 0 = none */
static uint8_t uid[12];
static uint8_t unsol_seq;
static uint8_t reboot_mode;          /* 0 none, 1 app, 2 vendor bootloader */
static uint32_t reboot_at;

/* What the previous stage left, read before we change anything. */
static uint32_t boot_csr, boot_rcc_cr, boot_rcc_cfgr, boot_acr;
static int hse;

/* RAM probe crash guard: survives the IWDG reset a bad probe would cause. */
#define RAMPROBE_MAGIC 0x52414D50u   /* 'RAMP' */
__attribute__((section(".noinit"))) static uint32_t ramprobe_marker;
static uint32_t ram_kb;              /* 0 = not probed */
static uint8_t ram_skipped;

/* ---- text helpers (no printf) ----------------------------------------- */

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_hex(char *p, uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--)
        *p++ = hex[(v >> (4 * i)) & 0xF];
    return p;
}

static char *put_dec(char *p, uint32_t n)
{
    char d[10];
    int k = 0;
    do {
        d[k++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n && k < 10);
    while (k)
        *p++ = d[--k];
    return p;
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
    midi_tx_sysex(buf, gsx_tx_end(&t));
    midi_tx_flush(50);
}

static void log_line(char *line, char *end)
{
    *end = 0;
    send_log(2, line, 0);
    sys_iwdg_kick();
}

/* ---- SRAM size probe -------------------------------------------------- */
#if PROBE_RAM
/* ARMv7-M: with FAULTMASK set (execution priority -1) and CCR.BFHFNMIGN,
 * a data bus fault on a load/store is ignored instead of escalating, and
 * still recorded in BFSR. ACTLR.DISDEFWBUF makes store faults precise, so
 * they are caught here rather than later. A word "exists" if no bus fault
 * was recorded and a write/read-back of two patterns works. Only addresses
 * above our own 20 KB RAM are touched. */
#define SCB_ACTLR REG32(0xE000E008u)
#define ACTLR_DISDEFWBUF (1u << 1)
#define CCR_BFHFNMIGN (1u << 8)
#define CFSR_BFSR_MASK 0x0000FF00u

static int ram_word_ok(uint32_t addr)
{
    volatile uint32_t *p = (volatile uint32_t *)addr;
    SCB_CFSR = CFSR_BFSR_MASK;       /* w1c */
    *p = 0x5AA5C33Cu;
    __asm__ volatile ("dsb" ::: "memory");
    uint32_t a = *p;
    *p = 0xA55A3CC3u;
    __asm__ volatile ("dsb" ::: "memory");
    uint32_t b = *p;
    __asm__ volatile ("dsb" ::: "memory");
    int fault = (SCB_CFSR & CFSR_BFSR_MASK) != 0;
    SCB_CFSR = CFSR_BFSR_MASK;
    return !fault && a == 0x5AA5C33Cu && b == 0xA55A3CC3u;
}

static void ram_probe(void)
{
    if (ramprobe_marker == RAMPROBE_MAGIC) {
        ram_skipped = 1;             /* it crashed us last time: don't retry */
        ramprobe_marker = 0;
        return;
    }
    ramprobe_marker = RAMPROBE_MAGIC;
    uint32_t actlr = SCB_ACTLR, ccr = SCB_CCR;
    SCB_ACTLR = actlr | ACTLR_DISDEFWBUF;
    SCB_CCR = ccr | CCR_BFHFNMIGN;
    __asm__ volatile ("dsb\n\tisb\n\tcpsid f" ::: "memory");
    uint32_t kb = 20;
    /* 1 KB steps, last word of each KB, up to 128 KB (no F1 has more). */
    while (kb < 128u && ram_word_ok(0x20000000u + kb * 1024u + 1020u))
        kb++;
    __asm__ volatile ("cpsie f\n\tdsb\n\tisb" ::: "memory");
    SCB_CCR = ccr;
    SCB_ACTLR = actlr;
    ram_kb = kb;
    ramprobe_marker = 0;
}
#endif

/* ---- the report ------------------------------------------------------- */

static void report(void)
{
    char s[64], *p;

    p = put_str(s, "probe fw ");
    p = put_dec(p, FW_VERSION_MAJOR);
    *p++ = '.';
    p = put_dec(p, FW_VERSION_MINOR);
    *p++ = '.';
    p = put_dec(p, FW_VERSION_PATCH);
    p = put_str(p, " hclk=");
    p = put_dec(p, sys_hclk_hz / 1000000u);
    p = put_str(p, "MHz hse=");
    p = put_dec(p, (uint32_t)hse);
    log_line(s, p);

    uint32_t id = DBGMCU_IDCODE;
    p = put_str(s, "idcode=");
    p = put_hex(p, id, 8);
    p = put_str(p, " dev=");
    p = put_hex(p, id & 0xFFFu, 3);
    p = put_str(p, " rev=");
    p = put_hex(p, id >> 16, 4);
    p = put_str(p, " cpuid=");
    p = put_hex(p, SCB_CPUID, 8);
    log_line(s, p);

    p = put_str(s, "flash=");
    p = put_dec(p, flash_size_kb());
    p = put_str(p, "KB page=");
    p = put_dec(p, BOARD_FLASH_PAGE);
    p = put_str(p, " ram=");
#if PROBE_RAM
    if (ram_skipped)
        p = put_str(p, "skipped(crashed before)");
    else {
        p = put_dec(p, ram_kb);
        p = put_str(p, "KB");
    }
#else
    p = put_str(p, "not probed");
#endif
    log_line(s, p);

    p = put_str(s, "uid=");
    for (uint32_t i = 0; i < 12u; i++)
        p = put_hex(p, uid[i], 2);
    log_line(s, p);

    uint32_t obr = FLASH_OBR;
    p = put_str(s, "obr=");
    p = put_hex(p, obr, 8);
    p = put_str(p, " rdp=");
    p = put_dec(p, (obr & FLASH_OBR_RDPRT) ? 1u : 0u);
    p = put_str(p, " operr=");
    p = put_dec(p, obr & FLASH_OBR_OPTERR);
    p = put_str(p, " wrpr=");
    p = put_hex(p, FLASH_WRPR, 8);
    log_line(s, p);

    p = put_str(s, "rcc_csr=");
    p = put_hex(p, boot_csr, 8);
    if (boot_csr & RCC_CSR_PORRSTF)
        p = put_str(p, " por");
    if (boot_csr & RCC_CSR_PINRSTF)
        p = put_str(p, " pin");
    if (boot_csr & RCC_CSR_SFTRSTF)
        p = put_str(p, " sft");
    if (boot_csr & RCC_CSR_IWDGRSTF)
        p = put_str(p, " iwdg");
    if (boot_csr & RCC_CSR_WWDGRSTF)
        p = put_str(p, " wwdg");
    if (boot_csr & RCC_CSR_LPWRRSTF)
        p = put_str(p, " lpwr");
    log_line(s, p);

    p = put_str(s, "boot rcc cr=");
    p = put_hex(p, boot_rcc_cr, 8);
    p = put_str(p, " cfgr=");
    p = put_hex(p, boot_rcc_cfgr, 8);
    p = put_str(p, " acr=");
    p = put_hex(p, boot_acr, 2);
    log_line(s, p);

    const volatile uint8_t *flag = (const volatile uint8_t *)UPDATE_FLAG_ADDR;
    p = put_str(s, "flag@0801FC00=");
    for (uint32_t i = 0; i < 8u; i++)
        p = put_hex(p, flag[i], 2);
    p = put_str(p, " chk@0801B3FE=");
    p = put_hex(p, *(const volatile uint16_t *)0x0801B3FEu, 4);
    log_line(s, p);
}

#if PROBE_DUMP_BOOTLOADER
/* 24 bytes per line: "dump AAAAAAAA:" + 48 hex digits = 62 chars. */
static uint32_t dump_at = 0x08000000u;

static void dump_step(void)
{
    if (dump_at >= 0x08002400u)
        return;
    char s[64], *p = put_str(s, "dump ");
    p = put_hex(p, dump_at, 8);
    *p++ = ':';
    uint32_t end = dump_at + 24u;
    if (end > 0x08002400u)
        end = 0x08002400u;
    for (; dump_at < end; dump_at++)
        p = put_hex(p, *(const volatile uint8_t *)dump_at, 2);
    log_line(s, p);
}
#endif

/* ---- requests --------------------------------------------------------- */

static void identity_reply(void)
{
    /* Universal Identity Reply: manufacturer 0x7D, family 'g' 'm',
     * model = board_id (as usb-log / app_proto). */
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
    gsx_put_u(t, 0, 2);                     /* max_chunk: no upload here */
    uint16_t caps = GSX_CAP_LOG;
    if (*(const volatile uint32_t *)GEMIFI_BOOT_MARK_ADDR == GEMIFI_BOOT_MAGIC)
        caps |= GSX_CAP_GEMIFI_BOOT;       /* uploads via the gemifi bootloader */
    gsx_put_u(t, caps, 2);
    gsx_put_packed(t, uid, sizeof(uid));
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
            st = GSX_BAD_STATE;             /* vendor protocol does uploads */
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

int main(void)
{
    /* Snapshot what the previous stage left before anything changes it. */
    boot_csr = sys_reset_cause();
    boot_rcc_cr = RCC_CR;
    boot_rcc_cfgr = RCC_CFGR;
    boot_acr = FLASH_ACR;

    sys_iwdg_start();
    hse = sys_clock_init();
    sys_tick_init();
    hal_delay_calibrate();
#if PROBE_RAM
    ram_probe();
#endif
    hal_uid(uid);
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < 24u; i++)
        serial[i] = hex[(uid[i / 2u] >> (i & 1u ? 0 : 4)) & 0xF];

    rx.buf = rx_buf;
    rx.cap = sizeof(rx_buf);
    usbd_init(&usb_desc);

    uint32_t t_cfg = 0, t_report = 0;
    int reported = 0;
    for (;;) {
        sys_iwdg_kick();
        usbd_poll();
        uint32_t now = sys_tick_ms();

        if (midi_rx_poll(&rx)) {
            handle_frame();
            gsx_rx_reset(&rx);
        }

        if (usbd_configured()) {
            if (!t_cfg)
                t_cfg = now | 1u;
            /* First report 1 s after configuration, then every 2 s. */
            if ((!reported && now - t_cfg >= 1000u) ||
                (reported && now - t_report >= REPORT_EVERY_MS)) {
                reported = 1;
                t_report = now;
                report();
            }
#if PROBE_DUMP_BOOTLOADER
            if (reported)
                dump_step();
#endif
        } else {
            t_cfg = 0;
        }

        if (reboot_mode && (int32_t)(now - reboot_at) >= 0)
            hal_reboot(reboot_mode == 2);
    }
}
