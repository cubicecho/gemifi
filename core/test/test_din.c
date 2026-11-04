/*
 * test_din.c: host test of the 5-pin DIN MIDI routing (core/src/din.c),
 * built twice: against din-port-test.yaml (din_midi.mode separate_port: the
 * jacks are USB cable 1) and din-merge-test.yaml (usb_merge: cable 0).
 * Simulated HAL: the DIN UART (a byte queue each way, with a size limit on
 * DIN Out), USB-MIDI events, pins for one button.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_din.h"
#include "params.h"

#if !GM_DIN
#error "test_din.c needs din-port-test.yaml or din-merge-test.yaml"
#endif
#define PORT (GM_DIN_MODE == GM_DIN_PORT)
#if PORT
#define CABLE 1u
#else
#define CABLE 0u
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static uint8_t level[64];
uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { level[pin] = (uint8_t)l; }
int hal_pin_read(uint16_t pin) { return level[pin]; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

/* USB: complete messages on cable 0 (the controller's), and raw events */
static uint8_t usb[4096];
static int usb_n;
void hal_midi_send(const uint8_t *m, uint32_t n) { if (usb_n + (int)n < (int)sizeof usb) { memcpy(usb + usb_n, m, n); usb_n += (int)n; } }
void hal_midi_send_cable(uint8_t c, const uint8_t *m, uint32_t n) { (void)c; hal_midi_send(m, n); }
static uint8_t ev[256][4];
static int ev_n;
void hal_midi_send_usb(const uint8_t e[4]) { if (ev_n < 256) memcpy(ev[ev_n++], e, 4); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* The DIN UART. dout: every byte written (the wire, in order). dq: bytes
 * of it still in the platform's TX ring of dq_size (stm32f1: 512), which
 * sends dq_rate thousandths of a byte a millisecond (31250 baud: 3125); 0
 * sends everything at each millisecond. */
static uint16_t open_tx, open_rx;
static uint32_t open_baud, open_flags;
static uint8_t dout[16384], din[4096];
static int dout_n, din_n, din_at;
static int dq, dq_size = 512;
static uint32_t dq_rate, dq_acc;
int hal_din_open(uint16_t tx, uint16_t rx, uint32_t baud, uint32_t flags) { open_tx = tx; open_rx = rx; open_baud = baud; open_flags = flags; return 1; }
int hal_din_write(const uint8_t *b, uint32_t n)
{
    if (dq + (int)n > dq_size || dout_n + (int)n > (int)sizeof dout)
        return 0;
    memcpy(dout + dout_n, b, n);
    dout_n += (int)n;
    dq += (int)n;
    return 1;
}
uint32_t hal_din_tx_queued(void) { return (uint32_t)dq; }
int hal_din_read(void) { return din_at < din_n ? din[din_at++] : -1; }

static void dq_send_ms(void)
{
    if (!dq_rate) {
        dq = 0;
        return;
    }
    for (dq_acc += dq_rate; dq_acc >= 1000u && dq > 0; dq_acc -= 1000u)
        dq--;
    if (dq == 0)
        dq_acc = 0;
}

/* ---- helpers ---------------------------------------------------------- */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        dq_send_ms();
        gm_poll();
    }
}

static void clear(void) { usb_n = ev_n = dout_n = 0; }

/* Long enough for DIN Out to send the status byte again (running status). */
static void idle(void)
{
    run_ms(300);
    clear();
}

static void local(uint8_t a, uint8_t b, uint8_t c, uint32_t n)
{
    const uint8_t m[3] = { a, b, c };
    gm_din_local(m, n);
}

#define DOUT_IS(...) do { const uint8_t want_[] = { __VA_ARGS__ }; \
        int ok_ = dout_is(want_, (int)sizeof want_); checks++; \
        if (!ok_) { fails++; printf("FAIL %s:%d: DIN Out\n", __FILE__, __LINE__); dump_dout(); } } while (0)

static void din_bytes(const uint8_t *b, int n)
{
    memcpy(din + din_n, b, (size_t)n);
    din_n += n;
    run_ms(2);
    while (din_at < din_n)
        run_ms(1);
}

static int dout_is(const uint8_t *want, int n)
{
    return dout_n == n && memcmp(dout, want, (size_t)n) == 0;
}

static int ev_is(int i, uint8_t e0, uint8_t e1, uint8_t e2, uint8_t e3)
{
    return i < ev_n && ev[i][0] == e0 && ev[i][1] == e1 && ev[i][2] == e2 && ev[i][3] == e3;
}

static void from_usb(uint8_t cable_cin, uint8_t a, uint8_t b, uint8_t c)
{
    const uint8_t e[4] = { cable_cin, a, b, c };
    gm_midi_rx_usb(e);
}

static void dump_dout(void)
{
    printf("  DIN Out:");
    for (int i = 0; i < dout_n; i++)
        printf(" %02X", dout[i]);
    printf("\n");
}

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    printf("din_midi.mode %s\n", PORT ? "separate_port" : "usb_merge");
    gm_init(0x00010000u, 0);
    CHECK(open_tx == 9 && open_rx == 10 && open_baud == 31250, "UART opened: PA9 / PA10 at 31250 (%u %u %u)",
          open_tx, open_rx, (unsigned)open_baud);
    CHECK(open_flags == (GM_DIN_DMA ? HAL_UART_DMA : 0u), "din_midi.dma -> HAL_UART_DMA (%u)", (unsigned)open_flags);
    run_ms(50);

    /* params from the YAML's defaults */
    const param_def_t *p = params_find(P_DIN_THRU);
    CHECK(p && p->def == 0 && gm_cfg.din_thru == 0, "midi.thru: off");
    p = params_find(P_DIN_OUT);
    CHECK(p && p->def == 1 && gm_cfg.din_out == 1, "midi.din_out: on");
    CHECK((params_find(P_DIN_USB) != 0) == !PORT, "midi.usb_to_din only with usb_merge");

    /* the controller's messages go to DIN Out too */
    clear();
    level[1] = 1;                       /* button: CC 64 127 */
    run_ms(20);
    {
        const uint8_t want[] = { 0xB0, 64, 127 };
        CHECK(dout_is(want, 3), "button: CC 64 on DIN Out");
        if (!dout_is(want, 3))
            dump_dout();
    }
    level[1] = 0;
    run_ms(20);
    clear();
    gm_cfg.din_out = 0;
    level[1] = 1;
    run_ms(20);
    CHECK(dout_n == 0 && usb_n >= 3, "midi.din_out off: USB only");
    level[1] = 0;
    run_ms(20);
    gm_cfg.din_out = 1;

    /* DIN In -> USB, with running status; no thru while midi.thru is off */
    clear();
    {
        const uint8_t b[] = { 0x91, 60, 100, 62, 90 };
        din_bytes(b, 5);
    }
    CHECK(ev_n == 2 && ev_is(0, CABLE << 4 | 9, 0x91, 60, 100) && ev_is(1, CABLE << 4 | 9, 0x91, 62, 90),
          "DIN In note on x2 (running status) -> USB cable %u (%d events)", CABLE, ev_n);
    CHECK(dout_n == 0, "no thru while midi.thru is off");

    /* thru */
    gm_cfg.din_thru = 1;
    clear();
    {
        const uint8_t b[] = { 0xC2, 5, 0xF8, 0xE0, 0, 64 };
        din_bytes(b, 6);
        const uint8_t want[] = { 0xC2, 5, 0xF8, 0xE0, 0, 64 };
        CHECK(dout_is(want, 6), "thru: program change, clock, pitch bend on DIN Out");
        CHECK(ev_n == 3 && ev_is(0, CABLE << 4 | 0xC, 0xC2, 5, 0) && ev_is(1, CABLE << 4 | 0xF, 0xF8, 0, 0) &&
              ev_is(2, CABLE << 4 | 0xE, 0xE0, 0, 64), "and on USB, CIN C / F / E");
    }
    /* system common: song position (CIN 3), tune request (CIN 5); no running status after */
    clear();
    {
        const uint8_t b[] = { 0xF2, 1, 2, 0xF6, 5 };
        din_bytes(b, 5);
        CHECK(ev_n == 2 && ev_is(0, CABLE << 4 | 3, 0xF2, 1, 2) && ev_is(1, CABLE << 4 | 5, 0xF6, 0, 0),
              "song position, tune request; a stray data byte after is dropped (%d events)", ev_n);
    }

    /* SysEx from DIN In */
    clear();
    {
        const uint8_t b[] = { 0xF0, 1, 2, 3, 0xF8, 4, 0xF7 };
        din_bytes(b, 7);
        const uint8_t want[] = { 0xF0, 1, 2, 3, 0xF8, 4, 0xF7 };
        CHECK(dout_is(want, 7), "thru SysEx, clock inside it passed through");
#if PORT
        /* its own cable: streamed as it comes, the clock between */
        CHECK(ev_n == 3 && ev_is(0, 0x14, 0xF0, 1, 2) && ev_is(1, 0x1F, 0xF8, 0, 0) && ev_is(2, 0x17, 3, 4, 0xF7),
              "SysEx to USB cable 1: CIN 4, clock, CIN 7 (%d events)", ev_n);
#else
        /* the main port: the SysEx goes out whole once it has ended */
        CHECK(ev_n == 3 && ev_is(0, 0x0F, 0xF8, 0, 0) && ev_is(1, 0x04, 0xF0, 1, 2) && ev_is(2, 0x07, 3, 4, 0xF7),
              "SysEx to USB cable 0 in one go after its F7 (%d events)", ev_n);
#endif
    }

    /* While the thru's SysEx goes out, the controller's messages wait */
    clear();
    {
        const uint8_t b[] = { 0xF0, 0x7D, 1 };
        din_bytes(b, 3);
        level[1] = 1;
        run_ms(20);
        const uint8_t mid[] = { 0xF0, 0x7D, 1 };
        CHECK(dout_is(mid, 3), "controller CC held back during the thru SysEx");
        const uint8_t e[] = { 2, 0xF7 };
        din_bytes(e, 2);
        const uint8_t want[] = { 0xF0, 0x7D, 1, 2, 0xF7, 0xB0, 64, 127 };
        CHECK(dout_is(want, 8), "then sent after its F7");
        if (!dout_is(want, 8))
            dump_dout();
        level[1] = 0;
        run_ms(20);
    }
    /* a SysEx that never ends is cut off with F7 after 500 ms */
    clear();
    {
        const uint8_t b[] = { 0xF0, 0x7D };
        din_bytes(b, 2);
        level[1] = 1;
        run_ms(20);
        run_ms(600);
        const uint8_t want[] = { 0xF0, 0x7D, 0xF7, 0xB0, 64, 127 };
        CHECK(dout_is(want, 6), "stalled SysEx ended with F7, then the held CC");
        if (!dout_is(want, 6))
            dump_dout();
        level[1] = 0;
        run_ms(20);
        clear();
        const uint8_t late[] = { 5, 0xF7, 0x90, 1, 1 };
        din_bytes(late, 5);
        const uint8_t want2[] = { 0x90, 1, 1 };
        CHECK(dout_is(want2, 3), "its late rest is dropped, the next message passes");
    }
    /* any status byte ends a SysEx */
    clear();
    {
        const uint8_t b[] = { 0xF0, 1, 0x80, 60, 0 };
        din_bytes(b, 5);
        const uint8_t want[] = { 0xF0, 1, 0xF7, 0x80, 60, 0 };
        CHECK(dout_is(want, 6), "a status byte inside a SysEx ends it (F7 added)");
    }
    gm_cfg.din_thru = 0;

    /* DIN Out full: whole messages or nothing */
    clear();
    dq_size = 4;
    {
        uint32_t d0 = gm_din_out_drops;
        const uint8_t m[] = { 0x90, 1, 2 };
        gm_din_local(m, 3);
        gm_din_local(m, 3);
        CHECK(dout_n == 3 && gm_din_out_drops == d0 + 1, "no room: the second message is dropped whole");
    }
    dq_size = 512;

    /* Running status: the status byte only when it changes; Note Off as
     * Note On velocity 0 after a Note On (velocity 0 or 64 only) */
    idle();
    local(0x90, 60, 100, 3);
    local(0x90, 62, 90, 3);
    local(0x80, 60, 0, 3);
    local(0x80, 62, 64, 3);
    local(0x80, 64, 30, 3);             /* a real release velocity: kept */
    local(0x80, 65, 30, 3);
    local(0xB0, 1, 10, 3);
    local(0xB0, 1, 20, 3);
    local(0xB1, 1, 20, 3);              /* another channel: another status */
    local(0xD1, 50, 0, 2);
    local(0xD1, 51, 0, 2);
    DOUT_IS(0x90, 60, 100, 62, 90, 60, 0, 62, 0, 0x80, 64, 30, 65, 30, 0xB0, 1, 10, 1, 20, 0xB1, 1, 20,
            0xD1, 50, 51);
    /* realtime between messages keeps it; between a status and its data too
     * (DIN In thru) */
    idle();
    local(0x90, 60, 100, 3);
    local(0xF8, 0, 0, 1);
    local(0x90, 62, 90, 3);
    DOUT_IS(0x90, 60, 100, 0xF8, 62, 90);
    gm_cfg.din_thru = 1;
    idle();
    {
        const uint8_t b[] = { 0x92, 0xF8, 60, 0xFA, 100, 62, 0xFC, 90 };
        din_bytes(b, 8);
    }
    DOUT_IS(0xF8, 0xFA, 0x92, 60, 100, 0xFC, 62, 90);
    /* SysEx and system common end it */
    idle();
    local(0x90, 60, 100, 3);
    {
        const uint8_t b[] = { 0xF0, 0x7D, 0xF7 };
        din_bytes(b, 3);
    }
    local(0x90, 62, 90, 3);
    {
        const uint8_t b[] = { 0xF3, 4 };
        din_bytes(b, 2);
    }
    local(0x90, 64, 80, 3);
    DOUT_IS(0x90, 60, 100, 0xF0, 0x7D, 0xF7, 0x90, 62, 90, 0xF3, 4, 0x90, 64, 80);
    gm_cfg.din_thru = 0;
    /* the status byte again after RS_REFRESH_MS (250 ms), idle or not */
    idle();
    for (int i = 0; i < 600; i++) {
        local(0xB0, 1, (uint8_t)(i & 0x7F), 3);
        run_ms(1);
    }
    {
        int st = 0;
        for (int i = 0; i < dout_n; i++)
            st += dout[i] == 0xB0;
        CHECK(st == 3 && dout_n == 600 * 2 + 3, "600 ms of CCs: the status byte at 0, 250 and 500 ms (%d, %d bytes)",
              st, dout_n);
    }
    clear();
    local(0xB0, 1, 0, 3);
    run_ms(260);
    local(0xB0, 1, 1, 3);
    DOUT_IS(1, 0, 0xB0, 1, 1);

#if PORT
    /* USB cable 1 -> DIN Out: short messages and SysEx */
    clear();
    from_usb(0x19, 0x92, 70, 80);
    from_usb(0x1F, 0xFA, 0, 0);
    from_usb(0x14, 0xF0, 0x43, 0x10);
    from_usb(0x16, 0x4C, 0xF7, 0);
    from_usb(0x15, 0xF6, 0, 0);
    {
        const uint8_t want[] = { 0x92, 70, 80, 0xFA, 0xF0, 0x43, 0x10, 0x4C, 0xF7, 0xF6 };
        CHECK(dout_is(want, 10), "USB port -> DIN Out: note, start, SysEx, tune request");
        if (!dout_is(want, 10))
            dump_dout();
    }
    /* the main port's messages stay off DIN */
    clear();
    from_usb(0x09, 0x90, 1, 1);
    CHECK(dout_n == 0, "USB cable 0 -> not DIN Out");
#else
    /* usb_merge: the main port's channel and realtime messages, when on */
    idle();
    from_usb(0x09, 0x90, 1, 1);
    CHECK(dout_n == 0, "midi.usb_to_din off: nothing");
    gm_cfg.din_usb = 1;
    from_usb(0x09, 0x90, 1, 1);
    from_usb(0x0F, 0xF8, 0, 0);
    from_usb(0x04, 0xF0, 0x7D, 0);
    from_usb(0x06, 0x00, 0xF7, 0);
    {
        const uint8_t want[] = { 0x90, 1, 1, 0xF8 };
        CHECK(dout_is(want, 4), "on: note and clock to DIN Out, never SysEx (the gemifi protocol's)");
        if (!dout_is(want, 4))
            dump_dout();
    }
    gm_cfg.din_usb = 0;
    /* a SysEx too long to merge is dropped */
    clear();
    {
        uint32_t d0 = gm_din_sysex_drops;
        uint8_t b[300];
        b[0] = 0xF0;
        for (int i = 1; i < 299; i++)
            b[i] = (uint8_t)(i & 0x7F);
        b[299] = 0xF7;
        din_bytes(b, 300);
        CHECK(ev_n == 0 && gm_din_sysex_drops == d0 + 1, "300-byte SysEx: not merged (%d events)", ev_n);
    }
#endif

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
