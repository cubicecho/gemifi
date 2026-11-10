/*
 * test_tm1628.c: host test of the TM1628 LED driver hub (vpin.c) with the
 * device core/test/tm1628-test.yaml: two chips sharing CLK and DIO, each
 * with its own STB. The simulated HAL decodes the bit stream on the pins
 * (LSB first, DIO taken on the CLK rising edge, STB low = one command) into
 * each chip's display RAM, and checks the timing rules it can see against
 * the time the delays ask for: a virtual clock in ns, moved by hal_delay_us
 * and by each hal_cycles() call (10 ns; hal_ns_to_cycles(ns) = ns), so the
 * waits on hal_cycles() take what they count. GM_TEST_NO_CYCLES=1 makes
 * hal_ns_to_cycles 0 (no cycle counter): the bits are then timed with
 * hal_delay_us, as they are without hal_pin_bsrr. The rgb_serial hub's
 * messages are caught at hal_serial_write.
 *
 * Built with GM_TEST_STORE (vpin.c's TM_STORE calls gm_test_store); with
 * GM_TEST_BSRR=1 in the environment, hal_pin_bsrr gives the TM1628 a
 * set/reset register per port, so CLK, DIO and STB go through the store
 * path, as on the STM32. Without it they go through hal_pin_write.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_presets.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "gm_feedback.h"
#include "gm_vpin.h"

#if !defined(GM_TM1628_RAM) || GM_TM1628_RAM != 32
#error "test_tm1628.c needs the tm1628-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

#define PIN_STB_A 15            /* PA15 */
#define PIN_CLK   42            /* PC10 */
#define PIN_DIO   43            /* PC11 */
#define PIN_STB_B 44            /* PC12 */
#define PIN_BTN   16            /* PB0  */

static uint32_t now_ms;
static uint8_t level[64];
static uint8_t mode[64];
static int in_tick;             /* inside gm_scan_tick */
static int tick_writes;         /* TM pins written from the scan tick */
static int dio_while_high;      /* DIO changed while CLK high inside a command */
static int bits_odd;            /* commands that weren't whole bytes */

/* Virtual time, ns: only the delays move it. */
static uint64_t t_ns;
static uint64_t t_clk_fall, t_clk_rise, t_dio, t_stb_rise[2];
static uint64_t min_low = ~0ull, min_high = ~0ull, min_period = ~0ull, min_setup = ~0ull,
                min_hold = ~0ull, min_clk_stb = ~0ull, min_stb_high = ~0ull, min_stb_clk = ~0ull;
static int had_rise;            /* a CLK rising edge in this command */
static uint64_t t_stb_fall;
#define MIN(m, v) do { uint64_t v_ = (v); if (v_ < (m)) (m) = v_; } while (0)

/* The store path: a set/reset register per port (A, B, C). */
static int use_bsrr;
static volatile uint32_t bsrr[3];
static int stores;              /* TM pin changes by a store */
static int slow_writes;         /* TM pin changes by hal_pin_write, after gm_init */
static int init_done;

/* MIDI handed to the HAL and not yet polled out: work done meanwhile (an ADC
 * read, a TM1628 pin, a serial LED message) would delay the packet. */
static int midi_pending, midi_sent, late_work;
static void work(void) { late_work += midi_pending; }

/* One chip as the pins see it. */
typedef struct {
    uint16_t stb;
    int active;                 /* STB low */
    uint8_t buf[32];            /* bytes of this command */
    int nbits;
    uint8_t ram[16];
    int auto_inc;               /* last data command 0x40 (1) / 0x44 (0) */
    int disp_mode;              /* last display-mode command, -1 = none */
    int ctl;                    /* last display control, -1 = none */
    int ram_writes;             /* commands that wrote RAM */
    int cmds;
    int saw_auto;               /* a 0x40 data command was ever sent */
    int bytes;                  /* bytes on the bus, every command */
} chip_t;

static chip_t chip[2] = { { .stb = PIN_STB_A }, { .stb = PIN_STB_B } };

/* Every command on the bus as hex, "A:40|A:C4 20|": chip, then its bytes. */
static char bus_log[4096];
static size_t bus_len;

static void log_cmd(const chip_t *c, int n)
{
    if (bus_len + 4 + 3 * (size_t)n >= sizeof bus_log)
        return;
    bus_len += (size_t)sprintf(bus_log + bus_len, "%c:", c == &chip[0] ? 'A' : 'B');
    for (int i = 0; i < n; i++)
        bus_len += (size_t)sprintf(bus_log + bus_len, i ? " %02X" : "%02X", c->buf[i]);
    bus_log[bus_len++] = '|';
    bus_log[bus_len] = 0;
}

static void bus_clear(void)
{
    bus_len = 0;
    bus_log[0] = 0;
}

static void chip_end(chip_t *c)
{
    int n = c->nbits / 8;
    c->cmds++;
    c->bytes += n;
    log_cmd(c, n);
    if (c->nbits % 8)
        bits_odd++;
    if (n == 0)
        return;
    uint8_t cmd = c->buf[0];
    if ((cmd & 0xC0) == 0x00) {
        c->disp_mode = cmd & 3;
    } else if ((cmd & 0xC0) == 0x40) {
        c->auto_inc = !(cmd & 0x04);
        c->saw_auto |= c->auto_inc;
    } else if ((cmd & 0xC0) == 0x80) {
        c->ctl = cmd;
    } else {
        int a = cmd & 0x0F;
        for (int i = 1; i < n; i++) {
            if (a < 16)
                c->ram[a] = c->buf[i];
            if (c->auto_inc)
                a++;
        }
        if (n > 1)
            c->ram_writes++;
    }
}

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { t_ns += 1000ull * us; }
static int no_cycles;
uint32_t hal_cycles(void) { return (uint32_t)(t_ns += 10); }
uint32_t hal_ns_to_cycles(uint32_t ns) { return no_cycles ? 0 : ns; }
void hal_pin_mode(uint16_t pin, uint8_t m) { mode[pin] = m; }

static int tm_pin(uint16_t pin)
{
    return pin == PIN_STB_A || pin == PIN_STB_B || pin == PIN_CLK || pin == PIN_DIO;
}

/* A pin level change, and its timing inside a command (one chip's STB low:
 * the chips share CLK and DIO but never have STB low together). */
static void sim_write(uint16_t pin, int l)
{
    l = l != 0;
    if (in_tick && tm_pin(pin))
        tick_writes++;
    int was = level[pin];
    level[pin] = (uint8_t)l;
    for (int k = 0; k < 2; k++) {
        chip_t *c = &chip[k];
        if (pin == c->stb && was && !l) {
            c->active = 1;
            c->nbits = 0;
            memset(c->buf, 0, sizeof c->buf);
            if (t_stb_rise[k])
                MIN(min_stb_high, t_ns - t_stb_rise[k]);
            t_stb_fall = t_ns;
            had_rise = 0;
        } else if (pin == c->stb && !was && l && c->active) {
            c->active = 0;
            chip_end(c);
            if (had_rise)
                MIN(min_clk_stb, t_ns - t_clk_rise);
            t_stb_rise[k] = t_ns;
        } else if (pin == PIN_CLK && !was && l && c->active) {
            if (c->nbits < 8 * (int)sizeof c->buf && level[PIN_DIO])
                c->buf[c->nbits / 8] |= (uint8_t)(1u << (c->nbits % 8));
            c->nbits++;
            MIN(min_low, t_ns - t_clk_fall);
            MIN(min_setup, t_ns - t_dio);
            if (had_rise)
                MIN(min_period, t_ns - t_clk_rise);
            t_clk_rise = t_ns;
            had_rise = 1;
        } else if (pin == PIN_CLK && was && !l && c->active) {
            if (had_rise)
                MIN(min_high, t_ns - t_clk_rise);
            else
                MIN(min_stb_clk, t_ns - t_stb_fall);
            t_clk_fall = t_ns;
        } else if (pin == PIN_DIO && was != l && c->active) {
            if (level[PIN_CLK])
                dio_while_high++;
            if (had_rise)
                MIN(min_hold, t_ns - t_clk_rise);
            t_dio = t_ns;
        }
    }
    if (pin == PIN_DIO && was != l)
        t_dio = t_ns;
}

void hal_pin_write(uint16_t pin, int l)
{
    if (init_done && tm_pin(pin))
        slow_writes++;
    if (tm_pin(pin) && level[pin] != (l != 0))
        work();
    sim_write(pin, l);
}

volatile uint32_t *hal_pin_bsrr(uint16_t pin, uint32_t *bit)
{
    *bit = pin & 15u;
    return use_bsrr && pin < 48 ? &bsrr[pin >> 4] : 0;
}

/* vpin.c's TM_STORE: bits 0-15 set pins, 16-31 clear them. */
void gm_test_store(volatile uint32_t *reg, uint32_t w)
{
    uint32_t port = (uint32_t)(reg - bsrr);
    if (port >= 3)
        return;
    work();
    for (uint32_t b = 0; b < 16; b++) {
        if ((w >> b) & 1u) {
            sim_write((uint16_t)(port * 16 + b), 1);
            stores++;
        }
        if ((w >> (b + 16)) & 1u) {
            sim_write((uint16_t)(port * 16 + b), 0);
            stores++;
        }
    }
}

int hal_pin_read(uint16_t pin) { return level[pin]; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; work(); return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    (void)cable;
    (void)m;
    (void)n;
    midi_pending = 1;
    midi_sent++;
}
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) { midi_pending = 0; }
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* rgb_serial: what reached the "UART" */
static uint16_t ser_pin = 0xFFFF;
static uint32_t ser_baud;
static int ser_busy;                /* refuse writes, as if one were still going */
typedef struct { uint32_t t; uint8_t b[6]; uint32_t n; } ser_msg_t;
static ser_msg_t ser_log[256];
static int ser_n;

static uint32_t ser_flags;
int hal_serial_open(uint16_t tx_pin, uint32_t baud, uint32_t flags)
{
    ser_pin = tx_pin;
    ser_baud = baud;
    ser_flags = flags;
    return 1;
}

#ifdef RGB_ACK_TEST
/* The simulated pad board: answers each LED message with 0x40 + LED once it
 * is up (BOARD_UP_MS), unless told to lose the next one. */
#define BOARD_UP_MS 300u
static int board_drop;              /* lose this many LED messages */
static uint8_t ack_q[64];
static int ack_in, ack_out;
#endif

int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n)
{
    work();
    if (tx_pin != ser_pin || ser_busy || n > 6)
        return 0;
    if (ser_n < 256) {
        ser_log[ser_n].t = now_ms;
        ser_log[ser_n].n = n;
        memcpy(ser_log[ser_n].b, b, n);
        ser_n++;
    }
#ifdef RGB_ACK_TEST
    if (n == 6 && now_ms >= BOARD_UP_MS) {
        if (board_drop)
            board_drop--;
        else if (ack_in - ack_out < (int)sizeof ack_q)
            ack_q[ack_in++ % (int)sizeof ack_q] = (uint8_t)(0x40u + (b[1] - 0x80u));
    }
#endif
    return 1;
}

int hal_serial_read(uint16_t tx_pin)
{
#ifdef RGB_ACK_TEST
    if (tx_pin == ser_pin && ack_out < ack_in)
        return ack_q[ack_out++ % (int)sizeof ack_q];
#endif
    (void)tx_pin;
    return -1;
}

/* Messages (not keepalives) since index from, and the smallest gap between
 * any two of them. */
static int ser_msgs(int from, uint32_t *min_gap)
{
    int k = 0;
    uint32_t last = 0, gap = 0xFFFFFFFFu;
    for (int i = from; i < ser_n; i++) {
        if (ser_log[i].n != 6)
            continue;
        if (k && ser_log[i].t - last < gap)
            gap = ser_log[i].t - last;
        last = ser_log[i].t;
        k++;
    }
    if (min_gap)
        *min_gap = gap;
    return k;
}

static int ser_last_is(uint8_t led, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = ser_n - 1; i >= 0; i--)
        if (ser_log[i].n == 6) {
            const uint8_t *m = ser_log[i].b;
            return m[0] == 0x02 && m[1] == 0x80 + led && m[2] == r && m[3] == g && m[4] == b && m[5] == 0x03;
        }
    return 0;
}

/* ---- helpers ---------------------------------------------------------- */

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        in_tick = 1;
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        in_tick = 0;
        now_ms++;
        gm_poll();
    }
}

static void usb(uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t ev[4] = { (uint8_t)(b0 >> 4), b0, b1, b2 };
    gm_midi_rx_usb(ev);
}

/* Every bit of chip c's bytes in mask (bit = byte) to on. */
static void set_bytes(uint8_t c, uint32_t mask, int on)
{
    gm_pin_t p = { 0 };
    p.hub = (uint8_t)(c + 1u);              /* hub 1 = leds_a, 2 = leds_b */
    for (uint16_t b = 0; b < 16 * 8; b++)
        if ((mask >> (b / 8)) & 1u) {
            p.num = b;
            vpin_write(p, on);
        }
}

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    for (int k = 0; k < 2; k++) {
        chip[k].disp_mode = -1;
        chip[k].ctl = -1;
    }
    level[PIN_STB_A] = level[PIN_STB_B] = 1;
    level[PIN_BTN] = 1;                 /* released (inverted, pull-up) */
    use_bsrr = getenv("GM_TEST_BSRR") != NULL;
    no_cycles = getenv("GM_TEST_NO_CYCLES") != NULL;
    printf("TM1628 pins: %s, bits timed by %s\n", use_bsrr ? "set/reset stores" : "hal_pin_write",
           use_bsrr && !no_cycles ? "hal_cycles" : "hal_delay_us");

    gm_init(0x01000000u, 0);
    CHECK(presets_rec_fits(), "a record of either scope fits presets.c's rec[]");
    init_done = 1;
    CHECK(mode[PIN_DIO] == HAL_OUT_OD && mode[PIN_CLK] == HAL_OUT && mode[PIN_STB_A] == HAL_OUT &&
          mode[PIN_STB_B] == HAL_OUT, "DIO open-drain, CLK and STBs push-pull");
    CHECK(level[PIN_STB_A] && level[PIN_STB_B] && level[PIN_CLK] && level[PIN_DIO], "bus idles high");

    /* gm_init: lamp test on (the first write: display mode if grids: is set,
     * 0x40, the whole RAM from C0, display on), then off: only the bytes the
     * lamp test lit, each a run of its own (A bytes 4 and 13, B bytes 0 and 5:
     * two or more unchanged bytes between them) */
    CHECK(chip[0].ram_writes == 3 && chip[1].ram_writes == 3, "lamp test: 1 + 2 writes each (%d, %d)",
          chip[0].ram_writes, chip[1].ram_writes);
    CHECK(chip[0].disp_mode == 3 && chip[1].disp_mode == -1, "grids: 7 sends mode 3 to A only");
    CHECK(chip[0].saw_auto && chip[1].saw_auto, "first write: data command 0x40 (auto-increment)");
    CHECK(chip[0].auto_inc && chip[1].auto_inc, "changed bytes: auto-increment too");
    CHECK(strcmp(bus_log,
                 "A:03|A:40|A:C0 00 00 00 00 20 00 00 00 00 00 00 00 00 07|A:8B|"
                 "B:40|B:C0 00 00 00 00 00 80|B:8F|"
                 "A:40|A:C4 00|A:CD 00|"
                 "B:40|B:C0 01|B:C5 00|") == 0,
          "start-up bytes on the bus: %s", bus_log);
    CHECK(chip[0].ctl == 0x8B && chip[1].ctl == 0x8F, "display on, intensity 3 / default 7 (%02X %02X)",
          chip[0].ctl, chip[1].ctl);
    {
        uint8_t z[16] = { 0 };
        CHECK(memcmp(chip[0].ram, z, 14) == 0, "A: all off after the lamp test");
        CHECK(chip[1].ram[0] == 0x01 && chip[1].ram[5] == 0x00, "B: inverted LED off = bit set (%02X %02X)",
              chip[1].ram[0], chip[1].ram[5]);
    }

    /* nothing changes: nothing is written */
    int ca = chip[0].cmds, cb = chip[1].cmds;
    run_ms(200);
    CHECK(chip[0].cmds == ca && chip[1].cmds == cb, "no writes while nothing changes");

    /* cc_toggle button with an LED on A bit 37 = byte 4 bit 5: one changed
     * byte = 0x44, then C0|4 and the byte: 3 bytes on the bus */
    int wa = chip[0].ram_writes, ba = chip[0].bytes;
    level[PIN_BTN] = 0;
    run_ms(30);
    level[PIN_BTN] = 1;
    run_ms(30);
    CHECK(chip[0].ram[4] == 0x20, "button LED: A byte 4 = 0x20 (%02X)", chip[0].ram[4]);
    CHECK(chip[0].ram_writes == wa + 1 && chip[1].cmds == cb, "one write to A, none to B (%d, %d)",
          chip[0].ram_writes - wa, chip[1].cmds - cb);
    CHECK(chip[0].bytes - ba == 3, "one LED: 3 bytes on the bus (%d; the whole RAM was 17)",
          chip[0].bytes - ba);
    /* the button's CC goes to USB before any LED, ADC or TM1628 work */
    {
        int sent = midi_sent;
        late_work = 0;
        for (int i = 0; i < 10; i++) {
            level[PIN_BTN] = 0;
            run_ms(25 + (uint32_t)i);
            level[PIN_BTN] = 1;
            run_ms(26 + (uint32_t)i);       /* odd: each press lands on another phase of the 2 / 10 ms work */
        }
        CHECK(midi_sent - sent >= 10, "10 presses: CCs sent (%d)", midi_sent - sent);
        CHECK(late_work == 0, "nothing between a CC and hal_midi_poll (%d)", late_work);
        CHECK(chip[0].ram[4] == 0x20, "10 toggles: the LED as it was (%02X)", chip[0].ram[4]);
    }

    /* host feedback: Note On 40 lights B bit 47 = byte 5 bit 7 */
    usb(0x90, 40, 127);
    run_ms(20);
    CHECK(chip[1].ram[5] == 0x80 && chip[1].ram[0] == 0x01, "feedback LED: B byte 5 = 0x80 (%02X)",
          chip[1].ram[5]);
    usb(0x80, 40, 0);
    run_ms(20);
    CHECK(chip[1].ram[5] == 0x00, "Note Off: B byte 5 = 0");
    CHECK(chip[0].ram[4] == 0x20, "A unchanged");

    /* toggle off again */
    level[PIN_BTN] = 0;
    run_ms(30);
    level[PIN_BTN] = 1;
    run_ms(30);
    CHECK(chip[0].ram[4] == 0x00, "button LED off");

    /* RGB LED (A byte 13 bits 0-2): FB_LED_RGB picks the channels at least
     * half as bright as the brightest; FB_LED on is white */
    gm_led_script_rgb(3, 127, 0, 0, 0);
    run_ms(20);
    CHECK(chip[0].ram[13] == 0x01, "RGB red (%02X)", chip[0].ram[13]);
    gm_led_script_rgb(3, 100, 60, 10, 0);
    run_ms(20);
    CHECK(chip[0].ram[13] == 0x03, "RGB orange-ish = red + green (%02X)", chip[0].ram[13]);
    gm_led_script_rgb(3, 0, 0, 90, 0);
    run_ms(20);
    CHECK(chip[0].ram[13] == 0x04, "RGB blue (%02X)", chip[0].ram[13]);
    gm_led_script_rgb(3, 0, 127, 0, 1);
    run_ms(10);
    int on = 0, off = 0;
    for (int i = 0; i < 120; i++) {
        run_ms(10);
        on += chip[0].ram[13] == 0x02;
        off += chip[0].ram[13] == 0x00;
    }
    CHECK(on > 0 && off > 0 && on + off == 120, "RGB green blinking (%d on, %d off)", on, off);
    gm_led_script(3, 127);
    run_ms(20);
    CHECK(chip[0].ram[13] == 0x07, "FB_LED on: white (%02X)", chip[0].ram[13]);
    gm_led_script(3, 0);
    run_ms(20);
    CHECK(chip[0].ram[13] == 0x00, "FB_LED off");
#ifdef RGB_ACK_TEST
    /* fade: (chip A in the ack build): the chip's brightness follows the
     * brightest LED on it, in its 8 steps up to intensity 3; too dim for
     * the dimmest step is display off. Chip B has no fade:. */
    gm_led_script_rgb(3, 127, 0, 0, 0);
    run_ms(20);
    CHECK(chip[0].ctl == 0x8B, "fade: full red, intensity 3 (%02X)", chip[0].ctl);
    gm_led_script_rgb(3, 100, 0, 0, 0);
    run_ms(20);
    CHECK(chip[0].ctl == 0x8A && chip[0].ram[13] == 0x01, "fade: 100 of 127, step 2 (%02X %02X)",
          chip[0].ctl, chip[0].ram[13]);
    gm_led_script_rgb(3, 61, 0, 0, 0);
    run_ms(20);
    CHECK(chip[0].ctl == 0x89, "fade: 61 of 127, step 1 (%02X)", chip[0].ctl);
    gm_led_script_rgb(3, 40, 0, 0, 0);
    run_ms(20);
    CHECK(chip[0].ctl == 0x88, "fade: 40 of 127, the dimmest step (%02X)", chip[0].ctl);
    gm_led_script_rgb(3, 10, 0, 0, 0);
    run_ms(20);
    CHECK(chip[0].ctl == 0x80 && chip[0].ram[13] == 0x01, "fade: 10 of 127, display off (%02X %02X)",
          chip[0].ctl, chip[0].ram[13]);
    /* the button LED (A, lit full by its toggle) doesn't hold the chip at
     * full while a host colour is lit */
    level[PIN_BTN] = 0;
    run_ms(30);
    level[PIN_BTN] = 1;
    run_ms(30);
    CHECK(chip[0].ram[4] == 0x20 && chip[0].ctl == 0x80, "fade: a host colour wins over a lit button (%02X %02X)",
          chip[0].ram[4], chip[0].ctl);
    /* a breath: the host's colour changes every 50 ms while the button is
     * lit; the chip never jumps to full in between. Each change lands just
     * before an LED update (on the device it comes in in the same pass), so
     * the update sees it before the LED is drawn again. */
    {
        int full_seen = 0;
        for (int f = 0; f < 20; f++) {
            while (now_ms % 10u != 9u)
                run_ms(1);
            gm_led_script_rgb(3, (uint32_t)(40 + (f & 1) * 5), 0, 0, 0);
            for (int ms = 0; ms < 50; ms++) {
                run_ms(1);
                full_seen += chip[0].ctl == 0x8B;
            }
        }
        CHECK(full_seen == 0 && chip[0].ctl == 0x88, "fade: a changing host colour never jumps to full (%d ms, %02X)",
              full_seen, chip[0].ctl);
    }
    /* a blinking dim host colour: the chip stays at its step through the
     * blink's dark half too (the lit button doesn't pull it to full) */
    {
        gm_led_script_rgb(3, 40, 0, 0, 1);
        run_ms(20);
        int full_seen = 0;
        for (int ms = 0; ms < 1500; ms++) {
            run_ms(1);
            full_seen += chip[0].ctl == 0x8B;
        }
        CHECK(full_seen == 0 && chip[0].ctl == 0x88, "fade: a blinking dim colour doesn't pump the chip (%d ms full)",
              full_seen);
    }
    /* a host's black (FB_LED_RGB 0, 0, 0) is a colour at brightness 0: the
     * chip goes dark, not to full for the lit button */
    gm_led_script_rgb(3, 0, 0, 0, 0);
    run_ms(20);
    CHECK(chip[0].ctl == 0x80, "fade: a host's black: dark, not full (%02X)", chip[0].ctl);
    gm_led_script(3, 0);
    run_ms(20);
    CHECK(chip[0].ctl == 0x8B, "fade: FB_LED off is no colour: the lit button, full (%02X)", chip[0].ctl);
    level[PIN_BTN] = 0;
    run_ms(30);
    level[PIN_BTN] = 1;
    run_ms(30);
    gm_led_script(3, 127);
    run_ms(20);
    CHECK(chip[0].ctl == 0x8B, "fade: FB_LED on, full again (%02X)", chip[0].ctl);
    /* from a full level to a dim host colour: the colour's bytes go out with
     * the chip's new brightness, never at the old (full) one */
    {
        gm_led_script_rgb(3, 40, 0, 0, 0);
        int bad = 0;
        for (int ms = 0; ms < 30; ms++) {
            run_ms(1);
            bad += chip[0].ram[13] == 0x01 && chip[0].ctl == 0x8B;
        }
        CHECK(bad == 0 && chip[0].ram[13] == 0x01 && chip[0].ctl == 0x88,
              "fade: a dim host colour never shows at full (%d ms, %02X %02X)", bad, chip[0].ram[13], chip[0].ctl);
        gm_led_script(3, 127);
        run_ms(20);
    }
    CHECK(chip[1].ctl == 0x8F, "no fade: chip B stays at intensity 7 (%02X)", chip[1].ctl);
    gm_led_script(3, 0);
    run_ms(20);
#endif

    /* rgb_serial: opened on PA2 at 115200; every LED sent once at start
     * (all off, after the lamp test), one message per >= 5 ms; A5 every 50 ms */
    CHECK(ser_pin == 2 && ser_baud == 115200u, "serial opened on PA2 at 115200 (%u, %u)", ser_pin, ser_baud);
#ifdef RGB_ACK_TEST
    CHECK(ser_flags == (HAL_UART_DMA | HAL_UART_RX), "rgb_serial: by DMA, receiving (rx_pin)");
#else
    CHECK(ser_flags == HAL_UART_DMA, "rgb_serial: by DMA (the default)");
#endif
    uint32_t gap;
    int n0 = ser_msgs(0, &gap);
    CHECK(n0 >= 4, "every LED sent at start (%d messages)", n0);
    CHECK(gap >= 5u, "messages at least 5 ms apart (%u)", gap);
    int ka = 0;
    uint32_t ka_last = 0, ka_gap = 0;
    for (int i = 0; i < ser_n; i++)
        if (ser_log[i].n == 1 && ser_log[i].b[0] == 0xA5) {
            if (ka && ser_log[i].t - ka_last > ka_gap)
                ka_gap = ser_log[i].t - ka_last;
            ka_last = ser_log[i].t;
            ka++;
        }
    CHECK(ka >= 10 && ka_gap <= 55u, "A5 keepalive every 50 ms (%d, max gap %u)", ka, ka_gap);
#ifdef RGB_ACK_TEST
    /* rx_pin: before the board answers, unanswered LEDs are sent again with
     * a growing pause (little traffic while it starts); its first answer
     * (BOARD_UP_MS) means it's up: every LED is sent again then, not at 2 s. */
    if (now_ms < 2100u)
        run_ms(2100u - now_ms);
    int early = 0, rep = 0, seen[4] = { 0 };
    uint32_t first_up = 0xFFFFFFFFu;
    for (int i = 0; i < ser_n; i++) {
        if (ser_log[i].n != 6)
            continue;
        if (ser_log[i].t < BOARD_UP_MS)
            early++;
        else if (first_up == 0xFFFFFFFFu)
            first_up = ser_log[i].t;
        if (ser_log[i].t >= BOARD_UP_MS && ser_log[i].t < 1000u && ser_log[i].b[1] >= 0x80 &&
            ser_log[i].b[1] < 0x84 && !seen[ser_log[i].b[1] - 0x80]++)
            rep++;
    }
    CHECK(ser_n < 256, "serial log not full (%d)", ser_n);
    CHECK(early >= 1 && early <= 5, "before the board is up: a few tries only (%d LED messages)", early);
    CHECK(first_up < BOARD_UP_MS + 400u, "tried again soon after it's up (%u ms)", first_up);
    CHECK(rep == 4, "every LED sent once the board answers, before 1 s (%d of 4)", rep);
    {
        int late = 0;
        for (int i = 0; i < ser_n; i++)
            late += ser_log[i].n == 6 && ser_log[i].t >= 1000u && ser_log[i].t < 2100u;
        CHECK(late == 0, "all answered: nothing more, no 2 s repaint (%d)", late);
    }
#else
    /* The first colours are lost at power-on, so every LED goes out once
     * more at 2 s; nothing else is resent (a steady resend left the Code 61
     * pad board deaf after a cold start). */
    if (now_ms < 2100u)
        run_ms(2100u - now_ms);
    int rep = 0, seen[4] = { 0 };
    for (int i = 0; i < ser_n; i++)
        if (ser_log[i].n == 6 && ser_log[i].t >= 2000u && ser_log[i].t < 2100u && ser_log[i].b[1] >= 0x80 &&
            ser_log[i].b[1] < 0x84 && !seen[ser_log[i].b[1] - 0x80]++)
            rep++;
    CHECK(ser_n < 256, "serial log not full (%d)", ser_n);
    CHECK(rep == 4, "every LED sent once more at 2 s (%d of 4)", rep);
#endif
    int from = ser_n;
    run_ms(1000);
    CHECK(ser_msgs(from, 0) == 0, "nothing changes: nothing sent again (%d in 1 s)", ser_msgs(from, 0));
    /* LED 4 = serial LED 2: red, then white, then off. With white: (the
     * ack build: [255, 200, 220]) full white sends that. */
#ifdef RGB_ACK_TEST
#define WHITE_G 200
#define WHITE_B 220
#else
#define WHITE_G 0xFF
#define WHITE_B 0xFF
#endif
    gm_led_script_rgb(4, 127, 0, 0, 0);
    run_ms(20);
    CHECK(ser_last_is(2, 0xFF, 0, 0), "serial LED 2 red");
    /* Any colour: a DAW's 0..127 per channel goes out at 0..255 (an LED
     * that can show any colour), through gamma: and white: if set. */
    gm_led_script_rgb(4, 127, 64, 16, 0);
    run_ms(20);
#ifdef RGB_ACK_TEST
    CHECK(ser_last_is(2, 0xFF, 45, 3), "serial LED 2 orange, through gamma 2.2 and white");
#else
    CHECK(ser_last_is(2, 0xFF, 129, 32), "serial LED 2 orange, all 8 bits");
#endif
    gm_led_script_rgb(4, 127, 70, 16, 0);
    run_ms(20);
#ifndef RGB_ACK_TEST
    CHECK(ser_last_is(2, 0xFF, 141, 32), "a new colour at the same level is sent too");
#endif
    gm_led_script(4, 127);
    run_ms(20);
    CHECK(ser_last_is(2, 0xFF, WHITE_G, WHITE_B), "serial LED 2 on: white");
    /* busy UART: held back, then sent */
    ser_busy = 1;
    gm_led_script(4, 0);
    run_ms(20);
    CHECK(ser_last_is(2, 0xFF, WHITE_G, WHITE_B), "nothing new while the UART is busy");
    ser_busy = 0;
    run_ms(20);
    /* Off shows its full-colour idle look ("#000010"; through gamma 2.2
     * and white in the ack build: the faintest blue, never 0). */
#ifdef RGB_ACK_TEST
    CHECK(ser_last_is(2, 0, 0, 1), "serial LED 2 off once the UART is free: its idle look, dim blue");
#else
    CHECK(ser_last_is(2, 0, 0, 0x10), "serial LED 2 off once the UART is free: its idle look, dim blue");
#endif
#ifdef RGB_ACK_TEST
    /* A lost message isn't answered: it goes out again. */
    {
        run_ms(100);
        int lost_from = ser_n;
        board_drop = 1;
        gm_led_script(4, 127);
        run_ms(150);
        int twice = 0;
        for (int i = lost_from; i < ser_n; i++)
            twice += ser_log[i].n == 6 && ser_log[i].b[1] == 0x82 && ser_log[i].b[2] == 0xFF &&
                     ser_log[i].b[3] == WHITE_G;
        CHECK(twice == 2, "lost, not answered: sent again (%d times)", twice);
        int idle_from = ser_n;
        run_ms(500);
        CHECK(ser_msgs(idle_from, 0) == 0, "answered: nothing more (%d)", ser_msgs(idle_from, 0));
    }
#endif
    /* The board loses a write right after another, an A5 included. A stall
     * (here the busy UART; on the Code 61 the start-up) leaves the A5 and an
     * LED both due: they must not go out back to back, nor stay 50 ms
     * apart in step afterwards, or every resend is lost. */
    {
        int busy_end = ser_n;
        ser_busy = 1;
        run_ms(120);                        /* the A5 and a resend both overdue */
        ser_busy = 0;
        run_ms(300);
        uint32_t any_gap = 0xFFFFFFFFu;
        for (int i = busy_end + 1; i < ser_n; i++)
            if (ser_log[i].t - ser_log[i - 1].t < any_gap)
                any_gap = ser_log[i].t - ser_log[i - 1].t;
        CHECK(ser_n < 256, "serial log not full (%d)", ser_n);
        CHECK(any_gap >= 5u, "after a stall: every write, A5 included, 5 ms after the last (%u)", any_gap);
    }

    /* A display-sized change: bytes 0-11 of A, but not byte 4 (the LEDs').
     * Cut by the 4-byte budget (0x40, an address, 2 bytes) and taken up at
     * its next changed address: 6 passes. */
    {
        int passes = 0, most = 0;
        uint8_t want[16];
        set_bytes(0, 0x0FEFu, 1);           /* bytes 0-3, 5-11 */
        bus_clear();
        while (passes < 20) {
            int before = chip[0].bytes;
            vpin_poll_tm1628();
            int d = chip[0].bytes - before;
            if (d == 0)
                break;
            if (d > most)
                most = d;
            passes++;
        }
        for (int k = 0; k < 12; k++)
            want[k] = k == 4 ? chip[0].ram[4] : 0xFF;
        CHECK(memcmp(chip[0].ram, want, 12) == 0, "11 changed bytes all reach chip A");
        CHECK(passes == 6 && most == 4, "6 passes, at most 4 bus bytes each (%d, %d)", passes, most);
        CHECK(strcmp(bus_log, "A:40|A:C0 FF FF|A:40|A:C2 FF FF|A:40|A:C5 FF FF|A:40|A:C7 FF FF|"
                              "A:40|A:C9 FF FF|A:40|A:CB FF|") == 0,
              "the run, split: %s", bus_log);
        set_bytes(0, 0x0FEFu, 0);
        bus_clear();
        vpin_flush_tm1628();                /* all at once: one run of 12 */
        CHECK(strcmp(bus_log, "A:40|A:C0 00 00 00 00 00 00 00 00 00 00 00 00|") == 0,
              "a full flush of 11 bytes is one auto-increment write: %s", bus_log);
    }

    /* Two runs: bytes 0-3 and 8-10 (four unchanged bytes between): one
     * command each when the budget allows (a flush), else 2 bytes a pass. */
    {
        set_bytes(0, 0x070Fu, 1);
        bus_clear();
        vpin_flush_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:C0 FF FF FF FF|A:C8 FF FF FF|") == 0, "two runs: %s", bus_log);
        set_bytes(0, 0x070Fu, 0);
        bus_clear();
        for (int i = 0; i < 4; i++)
            vpin_poll_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:C0 00 00|A:40|A:C2 00 00|A:40|A:C8 00 00|A:40|A:CA 00|") == 0,
              "by the budget, 4 passes: %s", bus_log);
        bus_clear();
        vpin_poll_tm1628();
        CHECK(bus_log[0] == 0, "then nothing: %s", bus_log);
    }

    /* Bytes 2 and 4: one unchanged byte between, so one run (C2 and 3 bytes,
     * what two runs would cost too, with one strobe less); bytes 2 and 5:
     * two runs. */
    {
        set_bytes(0, 0x0014u, 1);
        bus_clear();
        vpin_flush_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:C2 FF 00 FF|") == 0, "gap of one: one run (%s)", bus_log);
        /* by the budget: no room for byte 4 after byte 3, so byte 3 isn't
         * rewritten for nothing */
        set_bytes(0, 0x0014u, 0);
        bus_clear();
        vpin_poll_tm1628();
        vpin_poll_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:C2 00|A:40|A:C4 00|") == 0, "gap of one, by the budget (%s)", bus_log);
        set_bytes(0, 0x0014u, 1);
        vpin_flush_tm1628();
        set_bytes(0, 0x0014u, 0);
        set_bytes(0, 0x0024u, 1);
        bus_clear();
        vpin_flush_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:C4 00 FF|") == 0, "2 stays, 4 off, 5 on (%s)", bus_log);
        set_bytes(0, 0x0024u, 0);
        bus_clear();
        vpin_flush_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:C2 00|A:C5 00|") == 0, "gap of two: two runs (%s)", bus_log);
        /* byte 12, next to the last (13) */
        set_bytes(0, 0x1000u, 1);
        bus_clear();
        vpin_flush_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:CC FF|") == 0, "byte 12 (%s)", bus_log);
        CHECK(chip[0].ram[13] == 0x00, "byte 13 untouched");
        set_bytes(0, 0x1000u, 0);
        vpin_flush_tm1628();
        /* chip B: its last bytes, 4 and 5 */
        set_bytes(1, 0x0030u, 1);
        bus_clear();
        vpin_poll_tm1628();
        CHECK(strcmp(bus_log, "B:40|B:C4 FF FF|") == 0, "B bytes 4-5 (%s)", bus_log);
        set_bytes(1, 0x0030u, 0);
        vpin_flush_tm1628();
        CHECK(chip[1].ram[4] == 0x00 && chip[1].ram[5] == 0x00, "B back off");
    }

    /* Both chips change: one budget for the pass, A first, B's turn next. */
    {
        set_bytes(0, 0x003Fu, 1);           /* A 0-5: 3 passes of 0x40, an address and 2 bytes */
        set_bytes(1, 0x0006u, 1);           /* B 1-2: 4 */
        bus_clear();
        for (int i = 0; i < 3; i++)
            vpin_poll_tm1628();
        CHECK(strcmp(bus_log, "A:40|A:C0 FF FF|A:40|A:C2 FF FF|A:40|A:C4 FF FF|") == 0,
              "A first, 3 passes (%s)", bus_log);
        bus_clear();
        vpin_poll_tm1628();
        CHECK(strcmp(bus_log, "B:40|B:C1 FF FF|") == 0, "then B (%s)", bus_log);
        set_bytes(0, 0x003Fu, 0);
        set_bytes(1, 0x0006u, 0);
        vpin_flush_tm1628();
    }

    CHECK(tick_writes == 0, "the scan tick never touches the TM1628 pins (%d)", tick_writes);
    CHECK(dio_while_high == 0, "DIO only changes while CLK is low (%d)", dio_while_high);
    CHECK(bits_odd == 0, "every command is whole bytes (%d)", bits_odd);
    if (use_bsrr)
        CHECK(stores > 1000 && slow_writes == 0, "every TM1628 pin change a store (%d stores, %d writes)",
              stores, slow_writes);
    else
        CHECK(stores == 0 && slow_writes > 1000, "no store path without hal_pin_bsrr (%d, %d)", stores,
              slow_writes);
    /* The datasheet's limits, from the delays alone. */
    printf("CLK low %llu, high %llu, period %llu ns; DIO setup %llu, hold %llu ns; "
           "CLK-STB %llu, STB high %llu, STB-CLK %llu ns\n",
           (unsigned long long)min_low, (unsigned long long)min_high, (unsigned long long)min_period,
           (unsigned long long)min_setup, (unsigned long long)min_hold, (unsigned long long)min_clk_stb,
           (unsigned long long)min_stb_high, (unsigned long long)min_stb_clk);
    CHECK(min_low >= 400 && min_high >= 400, "PWCLK >= 400 ns (low %llu, high %llu)",
          (unsigned long long)min_low, (unsigned long long)min_high);
    CHECK(min_period >= 1000, "CLK <= 1 MHz (%llu ns a bit)", (unsigned long long)min_period);
    if (no_cycles || !use_bsrr)
        CHECK(min_period >= 2000, "the slow path: 1 us a phase (%llu ns a bit)",
              (unsigned long long)min_period);
    else
        CHECK(min_period <= 1100, "about 1 us a bit (%llu ns)", (unsigned long long)min_period);
    CHECK(min_setup >= 500, "DIO setup >= 500 ns, for an open-drain DIO to rise (%llu)",
          (unsigned long long)min_setup);
    CHECK(min_hold >= 100, "DIO hold >= 100 ns (%llu)", (unsigned long long)min_hold);
    CHECK(min_clk_stb >= 1000, "last CLK rise to STB rise >= 1 us (%llu)", (unsigned long long)min_clk_stb);
    CHECK(min_stb_high >= 1000, "PWSTB >= 1 us (%llu)", (unsigned long long)min_stb_high);
    CHECK(min_stb_clk >= 1000, "STB fall to the first CLK fall >= 1 us (%llu)", (unsigned long long)min_stb_clk);
    CHECK(level[PIN_STB_A] && level[PIN_STB_B] && level[PIN_CLK], "bus idles high");

    printf("test_tm1628: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
