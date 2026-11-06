/*
 * gemifi bootloader for Nuvoton NUC1xx (LDROM, 4 KB).
 *
 * Implements protocol/sysex.md: boot decision and handoff (section 6),
 * HELLO / REBOOT / FW_BEGIN / FW_DATA / FW_END / FW_ABORT (sections 4, 4.2)
 * over class-compliant USB-MIDI, image check (section 5).
 *
 * Build variants:
 *   default           runs from LDROM, writes the app into APROM.
 *   TEST_IN_APROM=1   runs from APROM (via the stock LDROM), always stays in
 *                     update mode, and writes uploads into DATA FLASH
 *                     (0x1F000, 4 KB) as a stand-in target, so the whole
 *                     USB/SysEx/flash/CRC path can be tested without
 *                     touching LDROM (bootloader-design.md 10, M3).
 *
 * Safety rules (do not relax):
 *   - Only offsets inside [0, size) of the target region are written, and
 *     target_write()/target_erase() re-check the absolute address range.
 *   - ISPCON.LDUEN and ISPCON.CFGUEN are never set (see fmc.c, sys.c), so
 *     the hardware also refuses LDROM and CONFIG writes.
 *   - The app is started only if the image header, vectors and CRC check out.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "nuc1xx.h"
#include "sys.h"
#include "fmc.h"
#include "crc32.h"
#include "gsx.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"
#include "board.h"

#define BOOT_VERSION GEMIFI_VERSION(0, 1, 1)
#define MAX_CHUNK    256u                /* bytes of data per FW_DATA      */
#define PACKED_MAX   (MAX_CHUNK + (MAX_CHUNK + 6u) / 7u)  /* 293 */
#define REBOOT_DELAY_MS 50u

#if defined(TEST_IN_APROM) && TEST_IN_APROM
#define TARGET_BASE  FLASH_DATA_BASE
#define TARGET_SIZE  FLASH_DATA_SIZE
#define PRODUCT      "gemifi boot test (" BOARD_NAME ")"
#else
#undef TEST_IN_APROM
#define TARGET_BASE  FLASH_APROM_BASE
#define TARGET_SIZE  FLASH_APROM_SIZE
#define PRODUCT      "gemifi boot (" BOARD_NAME ")"
#endif

/* ---- USB identity (sysex.md 7) ---------------------------------------- */
static const uint8_t dev_desc[] = { USB_MIDI_DEVICE_DESC(0x1209, 0x0001, 0x0100) };
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC };
static char serial[25];
static const char *const strings[] = { "gemifi", PRODUCT, serial };
static const usbd_desc_t usb_desc = {
    dev_desc, cfg_desc, sizeof(cfg_desc), 3, strings,
};

/* ---- state ------------------------------------------------------------ */
enum { FW_IDLE, FW_UPPER, FW_PAGE0, FW_DONE };

static uint8_t rx_buf[GSX_OVERHEAD + 4 + PACKED_MAX + 8];
static gsx_rx_t rx;                      /* set up in update_mode()       */
static uint8_t tx_buf[48];               /* last reply, kept for resends */
static uint16_t tx_len;
static uint32_t last_req;                /* 0x10000 | seq << 8 | cmd of the
                                            last completed request, 0 = none */
static uint32_t data_w[MAX_CHUNK / 4];   /* unpacked FW_DATA, word aligned */
static uint32_t uid[3];                 /* HELLO uid / serial, LE words  */

static uint8_t fw_state;
static uint32_t fw_size, fw_crc, fw_ver, fw_next;

static uint8_t reboot_mode;              /* 0 none, 1 app, 2 bootloader */
static uint32_t reboot_at;

/* ---- flash target ----------------------------------------------------- */

/* The one place that turns an offset into a flash address for writing.
 * Anything outside [TARGET_BASE, TARGET_BASE + TARGET_SIZE) is refused,
 * whatever the protocol layer let through. */
static int target_addr(uint32_t off, uint32_t *addr)
{
    if (off >= TARGET_SIZE || (off & 3u))
        return -1;
    *addr = TARGET_BASE + off;
    return 0;
}

static int target_erase(uint32_t off)
{
    uint32_t a;
    if ((off & (FLASH_PAGE_SIZE - 1u)) || target_addr(off, &a))
        return -1;
    return fmc_erase_page(a);
}

static int target_write(uint32_t off, uint32_t w)
{
    uint32_t a;
    if (target_addr(off, &a) || fmc_program(a, w))
        return -1;
    return fmc_read(a) == w ? 0 : -1;    /* read-back verify */
}

static uint32_t target_read(uint32_t off)
{
    return fmc_read(TARGET_BASE + off);
}

/* zlib CRC-32 of the n bytes of flash at addr (n a multiple of 4), with the
 * word at address skip read as 0. Shared by the image check and the device
 * ID record. */
static uint32_t flash_crc(uint32_t addr, uint32_t n, uint32_t skip)
{
    uint32_t c = CRC32_INIT;
    for (uint32_t a = addr; a < addr + n; a += 4)
        c = crc32_word(c, a == skip ? 0 : fmc_read(a));
    return ~c;
}

/* sysex.md 5: magic, board, size, SP in SRAM, reset vector odd and inside
 * the image, CRC over [0, size) with the crc field taken as 0. If want_size
 * is non-zero, size, CRC and fw_version must also equal the FW_BEGIN values
 * (sysex.md 4: they must match the image header). */
static int image_valid(uint32_t want_size, uint32_t want_crc, uint32_t want_ver)
{
    if (target_read(GEMIFI_IMG_HDR_OFFSET) != GEMIFI_IMG_MAGIC)
        return 0;
    uint32_t w = target_read(GEMIFI_IMG_HDR_OFFSET + 4);
    if ((w & 0xFFFFu) != GEMIFI_IMG_HDR_VERSION || (w >> 16) != BOARD_ID)
        return 0;
    uint32_t size = target_read(GEMIFI_IMG_HDR_OFFSET + 8);
    uint32_t crc = target_read(GEMIFI_IMG_CRC_OFFSET);
    if (size < GEMIFI_IMG_HDR_END || size > TARGET_SIZE || (size & 3u))
        return 0;
    if (want_size && (size != want_size || crc != want_crc ||
                      target_read(GEMIFI_IMG_HDR_OFFSET + 16) != want_ver))
        return 0;
    uint32_t sp = target_read(0), pc = target_read(4);
    if (sp <= SRAM_BASE || sp > SRAM_BASE + SRAM_SIZE)
        return 0;
    if (!(pc & 1u) || (pc & ~1u) >= size)
        return 0;
    return flash_crc(TARGET_BASE, size, TARGET_BASE + GEMIFI_IMG_CRC_OFFSET) == crc;
}

/* ---- protocol --------------------------------------------------------- */

static uint32_t cmd_hello(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 0)
        return GSX_BAD_LEN;
    gsx_put(t, 1);                       /* role: bootloader */
    gsx_put_u(t, BOARD_ID, 2);
    gsx_put_u(t, BOOT_VERSION, 5);
    gsx_put(t, GSX_PROTO_VERSION);
    gsx_put_u(t, MAX_CHUNK, 2);
    gsx_put_u(t, GSX_CAP_FW_UPLOAD, 2);
    gsx_put_packed(t, (const uint8_t *)uid, sizeof(uid));
    return GSX_OK;
}

static uint32_t cmd_reboot(const gsx_frame_t *f)
{
    if (f->len != 1)
        return GSX_BAD_LEN;
    if (f->payload[0] > 1)
        return GSX_BAD_CMD;
    reboot_mode = f->payload[0] + 1;
    reboot_at = sys_tick_ms() + REBOOT_DELAY_MS;
    return GSX_OK;
}

static uint32_t cmd_fw_begin(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (f->len != 17)
        return GSX_BAD_LEN;
    const uint8_t *p = f->payload;
    uint32_t size = gsx_get_u(p, 5);
    uint32_t crc = gsx_get_u(p + 5, 5);
    uint32_t board = gsx_get_u(p + 10, 2);
    fw_state = FW_IDLE;
    if (board != BOARD_ID)
        return GSX_BAD_BOARD;
    if (size < GEMIFI_IMG_HDR_END || size > TARGET_SIZE || (size & 3u))
        return GSX_BAD_ADDR;
    /* sysex.md 4.2 rule 1: erase page 0 first, so the app counts as
     * invalid from here until FW_END has checked the new image. */
    if (target_erase(0))
        return GSX_FLASH_ERR;
    fw_size = size;
    fw_crc = crc;
    fw_ver = gsx_get_u(p + 12, 5);
    if (size > FLASH_PAGE_SIZE) {
        fw_state = FW_UPPER;
        fw_next = FLASH_PAGE_SIZE;
    } else {
        fw_state = FW_PAGE0;
        fw_next = 0;
    }
    gsx_put_u(t, MAX_CHUNK, 2);
    return GSX_OK;
}

static uint32_t cmd_fw_data(const gsx_frame_t *f, gsx_tx_t *t)
{
    if (fw_state != FW_UPPER && fw_state != FW_PAGE0)
        return GSX_BAD_STATE;
    if (f->len < 4 + 2 || f->len > 4 + PACKED_MAX)
        return GSX_BAD_LEN;
    uint32_t off = gsx_get_u(f->payload, 4);
    int n = gsx_unpack(f->payload + 4, f->len - 4u, (uint8_t *)data_w);
    if (n <= 0 || (n & 3) || (uint32_t)n > MAX_CHUNK)
        return GSX_BAD_LEN;
    /* sysex.md 4.2 rules 2-3: in order, inside [0, size), within a page. */
    if (off != fw_next || off + (uint32_t)n > fw_size ||
        (off & (FLASH_PAGE_SIZE - 1u)) + (uint32_t)n > FLASH_PAGE_SIZE)
        return GSX_BAD_ADDR;
    if ((off & (FLASH_PAGE_SIZE - 1u)) == 0 && off != 0 && target_erase(off))
        return GSX_FLASH_ERR;
    for (uint32_t i = 0; i < (uint32_t)n; i += 4)
        if (target_write(off + i, data_w[i / 4]))
            return GSX_FLASH_ERR;
    fw_next = off + (uint32_t)n;
    if (fw_state == FW_UPPER && fw_next == fw_size) {
        fw_state = FW_PAGE0;             /* page 0 goes last */
        fw_next = 0;
    } else if (fw_state == FW_PAGE0 &&
               fw_next == (fw_size < FLASH_PAGE_SIZE ? fw_size : FLASH_PAGE_SIZE)) {
        fw_state = FW_DONE;
    }
    gsx_put_u(t, off, 4);
    return GSX_OK;
}

static uint32_t cmd_fw_end(const gsx_frame_t *f)
{
    if (f->len != 0)
        return GSX_BAD_LEN;
    if (fw_state != FW_DONE)
        return GSX_BAD_STATE;
    fw_state = FW_IDLE;
    return image_valid(fw_size, fw_crc, fw_ver) ? GSX_OK : GSX_BAD_IMAGE;
}

static void send(const uint8_t *p, uint32_t n)
{
    midi_tx_sysex(p, n);
    midi_tx_flush(200);
}

static void handle_frame(void)
{
    gsx_frame_t f;
    int pst = gsx_parse(&rx, &f);
    if (pst == GSX_NOT_OURS)
        return;
    /* sysex.md 1: a repeated seq for the command just completed gets the
     * same reply again without redoing the work. */
    uint32_t req = 0x10000u | (uint32_t)f.seq << 8 | f.cmd;
    if (pst == GSX_OK && req == last_req) {
        send(tx_buf, tx_len);
        return;
    }

    /* A frame-level error (pst) is answered from a scratch buffer, so the
     * cached reply of the last good request survives for a resend. Both
     * cases share one begin/end (size). */
    uint8_t err[GSX_OVERHEAD + 4];
    gsx_tx_t t;
    gsx_tx_begin(&t, pst == GSX_OK ? tx_buf : err, f.cmd | GSX_REPLY, f.seq);
    uint16_t status_at = t.n;
    gsx_put(&t, 0);
    uint32_t st = (uint32_t)pst;
    if (pst == GSX_OK) {
        switch (f.cmd) {
        case GSX_CMD_HELLO:    st = cmd_hello(&f, &t); break;
        case GSX_CMD_REBOOT:   st = cmd_reboot(&f); break;
        case GSX_CMD_FW_BEGIN: st = cmd_fw_begin(&f, &t); break;
        case GSX_CMD_FW_DATA:  st = cmd_fw_data(&f, &t); break;
        case GSX_CMD_FW_END:   st = cmd_fw_end(&f); break;
        case GSX_CMD_FW_ABORT:
            st = f.len ? GSX_BAD_LEN : GSX_OK;
            if (!f.len)
                fw_state = FW_IDLE;
            break;
        default:               st = GSX_BAD_CMD; break;
        }
    }
    if (st != GSX_OK)
        t.n = status_at + 1u;            /* status only on errors */
    t.buf[status_at] = (uint8_t)st;
    uint16_t n = gsx_tx_end(&t);
    if (pst == GSX_OK) {
        tx_len = n;
        last_req = req;
    }
    send(t.buf, n);
}

/* Device ID record of chips without a UID (written by the app, hal.c):
 * 'GDID', id[0..2], zlib crc32 over the first 4 words. Read only. */
#define DEVID_MAGIC 0x44494447u

static void make_serial(void)
{
    /* uid[] is little-endian words, so its bytes are the HELLO uid. If the
     * chip has no UID (NUC120BN), use the app's stored ID when its magic
     * and CRC match (skip = 0: no word in the record is skipped); else the
     * zeros from fmc_read_uid() stay. */
    if (fmc_read_uid(uid) != 0 &&
        fmc_read(FLASH_DEVID_ADDR) == DEVID_MAGIC &&
        flash_crc(FLASH_DEVID_ADDR, 16, 0) == fmc_read(FLASH_DEVID_ADDR + 16)) {
        for (uint32_t i = 0; i < 3; i++)
            uid[i] = fmc_read(FLASH_DEVID_ADDR + 4 + 4 * i);
    }
    /* three ID words, each printed most significant digit first (the same
     * string as the app's serial, app_proto_uid_hex()) */
    for (uint32_t i = 0; i < 24; i++) {
        uint32_t d = (uid[i / 8] >> (28 - 4 * (i & 7))) & 0xF;
        serial[i] = (char)(d + (d < 10 ? '0' : 'A' - 10));
    }
}

__attribute__((noreturn)) static void do_reboot(void)
{
    if (reboot_mode == 2) {
        MAILBOX->magic = MAILBOX_MAGIC_BOOT;
        MAILBOX->magic_inv = ~MAILBOX_MAGIC_BOOT;
    }
    usbd_disconnect();
    sys_delay_ms(10);
#ifdef TEST_IN_APROM
    sys_reset_to(0);    /* we live in APROM: come straight back here */
#else
    /* Both modes go back through the bootloader: it re-checks the app and
     * hands off only if the image is valid (mode 0), or stays (mode 1). */
    sys_reset_to(1);
#endif
}

static void update_mode(void)
{
    make_serial();
    rx.buf = rx_buf;
    rx.cap = sizeof(rx_buf);
    usbd_init(&usb_desc);
    uint32_t led_t = sys_tick_ms();
    uint32_t phase = 0;
    for (;;) {
        usbd_poll();
        if (midi_rx_poll(&rx)) {
            handle_frame();
            gsx_rx_reset(&rx);
        }
        uint32_t now = sys_tick_ms();
        /* LEDs: octave LEDs alternate at 2 Hz while waiting; both flash
         * together at 5 Hz during an upload. */
        uint32_t period = fw_state != FW_IDLE ? 100 : 250;
        if (now - led_t >= period) {
            led_t = now;
            phase ^= 1;
            board_led(LED_OCT_DOWN, phase);
            board_led(LED_OCT_UP, fw_state != FW_IDLE ? phase : !phase);
        }
        if (reboot_mode && (int32_t)(now - reboot_at) >= 0)
            do_reboot();
    }
}

int main(void)
{
    sys_clock_init();
    board_gpio_init();
    sys_tick_init();
    fmc_open();

    /* sysex.md 6: stay in update mode if the app asked for it ... */
    int stay = MAILBOX->magic == MAILBOX_MAGIC_BOOT &&
               MAILBOX->magic_inv == ~MAILBOX_MAGIC_BOOT;
    /* magic_inv then carries the reset flags, for the app's
     * diag.reset_reason (sysex.md 6): the app only sees the hand-off. */
    uint32_t rst = sys_reset_cause();
    MAILBOX->magic = 0;
    MAILBOX->magic_inv = rst;

    /* ... or the entry combo is held (3 samples over ~20 ms) ... */
    uint32_t held = BOARD_BOOT_COMBO;
    for (uint32_t i = 0; i < 3; i++) {
        if (i)
            sys_delay_ms(10);
        held &= board_buttons();
    }
    if (held == BOARD_BOOT_COMBO)
        stay = 1;

#ifndef TEST_IN_APROM
    /* ... or the boot-loop guard trips (bootloader-design.md 3.6): the app
     * was started MAILBOX_BOOT_TRIES times without clearing boot_count. SRAM
     * is random after a cold start, so the count only counts from then. */
    if (rst & RSTSRC_COLD) {
        MAILBOX->boot_count = 0;
        MAILBOX->reason = 0;
    }
    if (!stay) {
        if (MAILBOX->boot_count >= MAILBOX_BOOT_TRIES) {
            MAILBOX->reason = MAILBOX_REASON_BOOT_LOOP;
        } else if (image_valid(0, 0, 0)) {
            /* ... or the app is not valid. Otherwise hand off. USB was
             * never started on this path: nothing to disconnect. */
            MAILBOX->boot_count++;
            sys_reset_to(0);
        }
    }
    /* Staying: the next hand-off (REBOOT(0) or a reset) gets fresh tries. */
    MAILBOX->boot_count = 0;
#else
    (void)stay;         /* test build: always stays, there is no app */
#endif
    update_mode();
    return 0;
}
