/*
 * test_tm1628.c: host test of the TM1628 LED driver hub (vpin.c) with the
 * device core/test/tm1628-test.yaml: two chips sharing CLK and DIO, each
 * with its own STB. The simulated HAL decodes the bit stream on the pins
 * (LSB first, DIO taken on the CLK rising edge, STB low = one command) into
 * each chip's display RAM, and checks the timing rules it can see. The
 * rgb_serial hub's messages are caught at hal_serial_write.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_device_config.h"
#include "gm_feedback.h"
#include "gm_vpin.h"

#if !defined(GM_TM1628_RAM) || GM_TM1628_RAM != 22
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

static void chip_end(chip_t *c)
{
    int n = c->nbits / 8;
    c->cmds++;
    c->bytes += n;
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
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { mode[pin] = m; }

void hal_pin_write(uint16_t pin, int l)
{
    l = l != 0;
    if (in_tick && (pin == PIN_STB_A || pin == PIN_STB_B || pin == PIN_CLK || pin == PIN_DIO))
        tick_writes++;
    int was = level[pin];
    level[pin] = (uint8_t)l;
    for (int k = 0; k < 2; k++) {
        chip_t *c = &chip[k];
        if (pin == c->stb && was && !l) {
            c->active = 1;
            c->nbits = 0;
            memset(c->buf, 0, sizeof c->buf);
        } else if (pin == c->stb && !was && l && c->active) {
            c->active = 0;
            chip_end(c);
        } else if (pin == PIN_CLK && !was && l && c->active) {
            if (c->nbits < 8 * (int)sizeof c->buf && level[PIN_DIO])
                c->buf[c->nbits / 8] |= (uint8_t)(1u << (c->nbits % 8));
            c->nbits++;
        } else if (pin == PIN_DIO && was != l && c->active && level[PIN_CLK]) {
            dio_while_high++;
        }
    }
}

int hal_pin_read(uint16_t pin) { return level[pin]; }
int32_t hal_adc_read(uint16_t pin) { (void)pin; return -1; }
int hal_i2c_write(uint8_t a, const uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }
int hal_i2c_read(uint8_t a, uint8_t *p, uint32_t n) { (void)a; (void)p; (void)n; return -1; }

void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n) { (void)cable; (void)m; (void)n; }
void hal_midi_send(const uint8_t *m, uint32_t n) { (void)m; (void)n; }
void hal_midi_poll(void) {}
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

int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n)
{
    if (tx_pin != ser_pin || ser_busy || n > 6)
        return 0;
    if (ser_n < 256) {
        ser_log[ser_n].t = now_ms;
        ser_log[ser_n].n = n;
        memcpy(ser_log[ser_n].b, b, n);
        ser_n++;
    }
    return 1;
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

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    for (int k = 0; k < 2; k++) {
        chip[k].disp_mode = -1;
        chip[k].ctl = -1;
    }
    level[PIN_STB_A] = level[PIN_STB_B] = 1;
    level[PIN_BTN] = 1;                 /* released (inverted, pull-up) */

    gm_init(0x01000000u, 0);
    CHECK(mode[PIN_DIO] == HAL_OUT_OD && mode[PIN_CLK] == HAL_OUT && mode[PIN_STB_A] == HAL_OUT &&
          mode[PIN_STB_B] == HAL_OUT, "DIO open-drain, CLK and STBs push-pull");
    CHECK(level[PIN_STB_A] && level[PIN_STB_B] && level[PIN_CLK] && level[PIN_DIO], "bus idles high");

    /* gm_init: lamp test on (the first write: the whole RAM, auto-increment),
     * then off: only the bytes the lamp test lit, at fixed addresses (A bytes
     * 4 and 13, B bytes 0 and 5) */
    CHECK(chip[0].ram_writes == 3 && chip[1].ram_writes == 3, "lamp test: 1 + 2 writes each (%d, %d)",
          chip[0].ram_writes, chip[1].ram_writes);
    CHECK(chip[0].disp_mode == 3 && chip[1].disp_mode == -1, "grids: 7 sends mode 3 to A only");
    CHECK(chip[0].saw_auto && chip[1].saw_auto, "first write: data command 0x40 (auto-increment)");
    CHECK(!chip[0].auto_inc && !chip[1].auto_inc, "then 0x44 (fixed address) for changed bytes");
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

    /* rgb_serial: opened on PA2 at 115200; every LED sent once at start
     * (all off, after the lamp test), one message per >= 5 ms; A5 every 50 ms */
    CHECK(ser_pin == 2 && ser_baud == 115200u, "serial opened on PA2 at 115200 (%u, %u)", ser_pin, ser_baud);
    CHECK(ser_flags == HAL_UART_DMA, "rgb_serial: by DMA (the default)");
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
    int from = ser_n;
    run_ms(100);
    int again = ser_msgs(from, 0);
    CHECK(again >= 1 && again <= 3, "nothing changes: one LED sent again every 50 ms (%d in 100 ms)", again);
    /* a full round sends every LED again: a lost message heals */
    from = ser_n;
    run_ms(20 * 50 + 50);
    CHECK(ser_msgs(from, 0) >= 16, "every LED sent again within a round (%d)", ser_msgs(from, 0));
    /* LED 4 = serial LED 2: red, then white, then off */
    gm_led_script_rgb(4, 127, 0, 0, 0);
    run_ms(20);
    CHECK(ser_last_is(2, 0xFF, 0, 0), "serial LED 2 red");
    gm_led_script(4, 127);
    run_ms(20);
    CHECK(ser_last_is(2, 0xFF, 0xFF, 0xFF), "serial LED 2 on: white");
    /* busy UART: held back, then sent */
    ser_busy = 1;
    gm_led_script(4, 0);
    run_ms(20);
    CHECK(ser_last_is(2, 0xFF, 0xFF, 0xFF), "nothing new while the UART is busy");
    ser_busy = 0;
    run_ms(20);
    CHECK(ser_last_is(2, 0, 0, 0), "serial LED 2 off once the UART is free");

    /* A big change goes out over a few passes, at most 9 bus bytes each. */
    {
        gm_pin_t p = { 1, 0, 0 };           /* hub 1 = leds_a */
        for (uint16_t b = 0; b < 96; b++) { /* bytes 0-11: every bit (not the LEDs' bytes 4/13) */
            if (b / 8 == 4)
                continue;
            p.num = b;
            vpin_write(p, 1);
        }
        int passes = 0, most = 0;
        uint8_t want[16];
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
        CHECK(passes >= 3 && most <= 9, "spread over %d passes, at most %d bus bytes each", passes, most);
        for (uint16_t b = 0; b < 96; b++) {
            if (b / 8 == 4)
                continue;
            p.num = b;
            vpin_write(p, 0);
        }
        vpin_flush_tm1628();                /* all at once: the whole RAM is shorter here */
        CHECK(chip[0].ram[0] == 0 && chip[0].ram[11] == 0 && chip[0].auto_inc,
              "a full flush of 11 bytes uses one auto-increment write");
    }

    CHECK(tick_writes == 0, "the scan tick never touches the TM1628 pins (%d)", tick_writes);
    CHECK(dio_while_high == 0, "DIO only changes while CLK is low (%d)", dio_while_high);
    CHECK(level[PIN_STB_A] && level[PIN_STB_B] && level[PIN_CLK], "bus idles high");

    printf("test_tm1628: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
