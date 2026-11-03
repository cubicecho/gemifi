/*
 * gemifi bootloader for STM32F1 boards (M-Audio Code 61), at 0x08000000,
 * 9 KB. Port of bootloader/nuvoton-nuc1xx/main.c.
 *
 * Implements protocol/sysex.md: boot decision and handoff (section 6),
 * HELLO / REBOOT / FW_BEGIN / FW_DATA / FW_END / FW_ABORT (sections 4, 4.2)
 * over class-compliant USB-MIDI, image check (section 5). The app area is
 * 0x08002400-0x0801AFFF (GEMIFI_IMG_BASE..GEMIFI_IMG_LIMIT), the same as
 * under M-Audio's bootloader, so app builds don't change.
 *
 * Stays in update mode if:
 *   - the update flag is set at 0x0801FC00 (recovery.h). gemifi apps set it
 *     on REBOOT(1) and on their own reset-time combo; the stock M-Audio app
 *     sets the same flag when its updater asks. The flag is cleared here;
 *   - the entry combo is held at reset: top-row buttons 1 and 2 from the
 *     left (matrix row 16, PB8 + PB9, verified on hardware 2026-11-03);
 *   - there is no app to start (below).
 * Otherwise it starts the app: a valid gemifi image (header, vectors, CRC),
 * or, when there is no gemifi header at all, an image whose vectors look
 * sane (SP in RAM, reset vector inside the app area): a non-gemifi app such
 * as M-Audio's stock firmware. A gemifi header that fails its check is
 * never started.
 *
 * In update mode, holding the power button for 2 s switches the board off.
 *
 * Safety rules (do not relax):
 *   - Only [GEMIFI_IMG_BASE, GEMIFI_IMG_LIMIT) is ever erased or written,
 *     plus the update-flag page; target_addr() re-checks every address.
 *   - Page 0 of the app (vectors + header) is erased first and written last,
 *     so an interrupted upload leaves no startable app.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "stm32f1.h"
#include "sys.h"
#include "flash.h"
#include "crc32.h"
#include "recovery.h"
#include "gsx.h"
#include "usbd.h"
#include "usb_midi.h"
#include "gemifi_image.h"
#include "board.h"

#define BOOT_VERSION GEMIFI_VERSION(BOOT_VERSION_MAJOR, BOOT_VERSION_MINOR, BOOT_VERSION_PATCH)
#define MAX_CHUNK    256u                /* bytes of data per FW_DATA */
#define PACKED_MAX   (MAX_CHUNK + (MAX_CHUNK + 6u) / 7u)
#define REBOOT_DELAY_MS 50u
#define POWER_OFF_HOLD_MS 2000u

#define TARGET_BASE  GEMIFI_IMG_BASE
#define TARGET_SIZE  (GEMIFI_IMG_LIMIT - GEMIFI_IMG_BASE)
#define PAGE         BOARD_FLASH_PAGE
#define SRAM_BASE    0x20000000u
#define SRAM_SIZE    (20u * 1024u)

/* ---- the marker apps look for (gemifi_image.h) -------------------------- */
__attribute__((section(".gemifi_boot_mark"), used))
const struct gemifi_boot_mark gemifi_boot_mark = {
    .magic = GEMIFI_BOOT_MAGIC,
    .board_id = BOARD_ID,
    .mark_version = 1,
    .boot_version = BOOT_VERSION,
    .reserved = 0xFFFFFFFFu,
};

/* ---- USB identity (sysex.md 7) ---------------------------------------- */
static const uint8_t dev_desc[] = { USB_MIDI_DEVICE_DESC(0x1209, 0x0001, 0x0100) };
static const uint8_t cfg_desc[] = { USB_MIDI_CONFIG_DESC };
static char serial[25];
static const char *const strings[] = { "gemifi", "gemifi boot (" BOARD_NAME ")", serial };
static const usbd_desc_t usb_desc = {
    dev_desc, cfg_desc, sizeof(cfg_desc), 3, strings,
};

/* ---- state ------------------------------------------------------------ */
enum { FW_IDLE, FW_UPPER, FW_PAGE0, FW_DONE };

static uint8_t rx_buf[GSX_OVERHEAD + 4 + PACKED_MAX + 8];
static gsx_rx_t rx;
static uint8_t tx_buf[48];               /* last reply, kept for resends */
static uint16_t tx_len;
static uint32_t last_req;                /* 0x10000 | seq << 8 | cmd, 0 = none */
static uint32_t data_w[MAX_CHUNK / 4];   /* unpacked FW_DATA, word aligned */
static uint8_t uid[12];

static uint8_t fw_state;
static uint32_t fw_size, fw_crc, fw_ver, fw_next;

static uint8_t reboot_mode;              /* 0 none, 1 app, 2 bootloader */
static uint32_t reboot_at;

/* ---- flash target ----------------------------------------------------- */

/* The one place that turns an offset into a flash address for writing. */
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
    if ((off & (PAGE - 1u)) || target_addr(off, &a))
        return -1;
    return flash_erase_page(a);
}

static int target_write(uint32_t off, uint32_t w)
{
    uint32_t a;
    if (target_addr(off, &a) || flash_program_word(a, w))
        return -1;
    return flash_read(a) == w ? 0 : -1;
}

static uint32_t target_read(uint32_t off)
{
    return flash_read(TARGET_BASE + off);
}

/* zlib CRC-32 of n bytes of flash at addr, the word at skip read as 0. */
static uint32_t flash_crc(uint32_t addr, uint32_t n, uint32_t skip)
{
    uint32_t c = CRC32_INIT;
    for (uint32_t a = addr; a < addr + n; a += 4)
        c = crc32_word(c, a == skip ? 0 : flash_read(a));
    return ~c;
}

/* Initial SP and reset vector of the app look startable. */
static int vectors_sane(uint32_t limit)
{
    uint32_t sp = target_read(0), pc = target_read(4);
    if (sp <= SRAM_BASE || sp > SRAM_BASE + SRAM_SIZE || (sp & 3u))
        return 0;
    return (pc & 1u) && (pc & ~1u) >= TARGET_BASE && (pc & ~1u) < TARGET_BASE + limit;
}

/* sysex.md 5. If want_size is non-zero, size, CRC and fw_version must also
 * equal the FW_BEGIN values. */
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
    if (!vectors_sane(size))
        return 0;
    return flash_crc(TARGET_BASE, size, TARGET_BASE + GEMIFI_IMG_CRC_OFFSET) == crc;
}

/* Something to start: a valid gemifi image, or a non-gemifi app. */
static int app_startable(void)
{
    if (target_read(GEMIFI_IMG_HDR_OFFSET) == GEMIFI_IMG_MAGIC)
        return image_valid(0, 0, 0);
    return vectors_sane(TARGET_SIZE);
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
    gsx_put_packed(t, uid, sizeof(uid));
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
    if (size > PAGE) {
        fw_state = FW_UPPER;
        fw_next = PAGE;
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
        (off & (PAGE - 1u)) + (uint32_t)n > PAGE)
        return GSX_BAD_ADDR;
    if ((off & (PAGE - 1u)) == 0 && off != 0 && target_erase(off))
        return GSX_FLASH_ERR;
    for (uint32_t i = 0; i < (uint32_t)n; i += 4)
        if (target_write(off + i, data_w[i / 4]))
            return GSX_FLASH_ERR;
    fw_next = off + (uint32_t)n;
    if (fw_state == FW_UPPER && fw_next == fw_size) {
        fw_state = FW_PAGE0;             /* page 0 goes last */
        fw_next = 0;
    } else if (fw_state == FW_PAGE0 && fw_next == (fw_size < PAGE ? fw_size : PAGE)) {
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
    /* sysex.md 1: a repeated seq gets the same reply without redoing it. */
    uint32_t req = 0x10000u | (uint32_t)f.seq << 8 | f.cmd;
    if (pst == GSX_OK && req == last_req) {
        send(tx_buf, tx_len);
        return;
    }
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
        t.n = status_at + 1u;
    t.buf[status_at] = (uint8_t)st;
    uint16_t n = gsx_tx_end(&t);
    if (pst == GSX_OK) {
        tx_len = n;
        last_req = req;
    }
    send(t.buf, n);
}

/* ---- board ------------------------------------------------------------ */

/* Entry combo: matrix row 16 selected (PB3..PB7 low; JTAG off frees PB3/PB4),
 * columns PB8 + PB9 pulled up, both read low = held. Pins go back to reset
 * state afterwards. */
static int combo_held(void)
{
    RCC_APB2ENR |= RCC_APB2_AFIO | RCC_APB2_IOP(PORTB);
    AFIO_MAPR = (AFIO_MAPR & ~AFIO_MAPR_SWJ_MASK) | AFIO_MAPR_SWJ_NOJTAG;
    GPIO_BRR(PORTB) = 0x1Fu << 3;
    GPIO_CRL(PORTB) = (GPIO_CRL(PORTB) & 0x00000FFFu) | 0x11111000u;  /* PB3..7 out */
    GPIO_BSRR(PORTB) = 0xFF00u;                                         /* pull-ups */
    GPIO_CRH(PORTB) = 0x88888888u;                                      /* PB8..15 in */
    int held = 1;
    for (uint32_t i = 0; i < 3u && held; i++) {
        sys_delay_ms(i ? 10u : 1u);
        if (GPIO_IDR(PORTB) & ((1u << 8) | (1u << 9)))
            held = 0;
    }
    GPIO_CRH(PORTB) = 0x44444444u;
    GPIO_CRL(PORTB) = (GPIO_CRL(PORTB) & 0x00000FFFu) | 0x44444000u;
    GPIO_BRR(PORTB) = 0xFFF8u;
    return held;
}

static void make_serial(void)
{
    const volatile uint8_t *u = (const volatile uint8_t *)UID_BASE;
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < 12u; i++)
        uid[i] = u[i];
    for (uint32_t i = 0; i < 24u; i++)
        serial[i] = hex[(uid[i / 2u] >> (i & 1u ? 0 : 4)) & 0xF];
}

/* Start the app as M-Audio's bootloader did: MSP from its vector table,
 * then its reset handler. Called before USB or the PLL were touched. */
__attribute__((noreturn)) static void start_app(void)
{
    SYST_CSR = 0;
    uint32_t sp = target_read(0), pc = target_read(4);
    __asm__ volatile (
        "msr msp, %0\n\t"
        "bx %1\n\t"
        :: "r" (sp), "r" (pc) : "memory");
    __builtin_unreachable();
}

__attribute__((noreturn)) static void do_reboot(void)
{
    /* Both modes reset through here; REBOOT(1) sets the update flag so the
     * next pass stays, REBOOT(0) starts the app if it is valid. */
    if (reboot_mode == 2)
        recovery_request_update();
    usbd_disconnect();
    sys_delay_ms(10);
    sys_reset();
}

__attribute__((noreturn)) static void power_off(void)
{
    usbd_disconnect();
    GPIO_BRR(PORTC) = 1u << BOARD_POWER_LATCH_PIN;   /* off once the button is released */
    for (;;) {
    }
}

__attribute__((noreturn)) static void update_mode(void)
{
    sys_clock_init();
    sys_tick_init();
    make_serial();
    flash_unlock();
    rx.buf = rx_buf;
    rx.cap = sizeof(rx_buf);
    usbd_init(&usb_desc);
    uint32_t btn_since = 0;
    for (;;) {
        usbd_poll();
        if (midi_rx_poll(&rx)) {
            handle_frame();
            gsx_rx_reset(&rx);
        }
        uint32_t now = sys_tick_ms();
        /* Power button held 2 s (not during an upload): switch off. Pressed
         * at power-on counts only once it has been released. */
        int pressed = !(GPIO_IDR(PORTC) & (1u << BOARD_POWER_BTN_PIN));
        if (!pressed)
            btn_since = now | 1u;
        else if (btn_since && fw_state == FW_IDLE && now - btn_since >= POWER_OFF_HOLD_MS)
            power_off();
        if (reboot_mode && (int32_t)(now - reboot_at) >= 0)
            do_reboot();
    }
}

int main(void)
{
    /* Power is already latched (board_gpio_init from Reset_Handler). SysTick
     * on the 8 MHz HSI is enough for the decision. */
    sys_tick_init();

    int stay = 0;
    if (recovery_update_requested()) {
        stay = 1;
        flash_unlock();
        flash_erase_page(UPDATE_FLAG_ADDR);      /* one-shot request */
        flash_lock();
    }
    if (combo_held())
        stay = 1;
    if (!stay && app_startable())
        start_app();
    update_mode();
}
