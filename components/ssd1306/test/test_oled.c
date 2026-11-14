/*
 * test_oled.c: host test of the ssd1306 display platform (SSD1306 / SH1106
 * OLEDs, components/ssd1306 on components/display) and text displays
 * (core/src/display.c) with oled-test.yaml here, on
 * a fake bus: each panel decodes the I2C writes (control bytes, commands,
 * page-mode data) into its RAM, and the test reads the pixels back.
 *   oled (0x3C, given as 0x78): big, line 1 at scale 2 (10 characters, the
 *     last message); chan, line 6 from x 4, 8 characters (the channel);
 *   small (SH1106, 0x3D, turned, contrast 64): host, line 0 (a host's text).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_hal.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_display.h"
#include "gm_feedback.h"

#if !(GM_N_HUBS == 0 && GM_N_SCREENS == 2 && GM_N_ANALOGS == 1 && GM_N_DISPLAYS == 3)
#error "test_oled.c needs the oled-test.yaml config"
#endif

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- simulated hardware ---------------------------------------------- */

static uint32_t now_ms;
static int32_t adc = 0;

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_pin_mode(uint16_t pin, uint8_t m) { (void)pin; (void)m; }
void hal_pin_write(uint16_t pin, int l) { (void)pin; (void)l; }
int hal_pin_read(uint16_t pin) { (void)pin; return 1; }
const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask) { (void)pin; (void)mask; return 0; }
volatile uint32_t *hal_pin_out_reg(uint16_t pin) { (void)pin; return 0; }
int32_t hal_adc_read(uint16_t pin) { return pin == 34 ? adc : -1; }

/* A panel: page-mode RAM (132 columns, as the SH1106's), what it was told. */
typedef struct {
    uint8_t addr;
    uint8_t ram[8][132];
    uint32_t page, col;
    int on, ons, offs;                  /* display on now; 0xAF / 0xAE seen */
    int contrast, remap, scan, pump, dcdc, page_mode, mux;
    int writes, data_writes, max_len;   /* transactions, those with data, the longest */
    int drawn_before_on;                /* data written while the display was off */
} panel_t;
static panel_t panel[2] = { { .addr = 0x3C, .contrast = -1 }, { .addr = 0x3D, .contrast = -1 } };
static int bad_addr;

static panel_t *panel_at(uint8_t a)
{
    for (int i = 0; i < 2; i++)
        if (panel[i].addr == a)
            return &panel[i];
    return 0;
}

/* A command byte; arg = its argument for the two-byte ones, else -1. */
static int cmd_has_arg(uint8_t c)
{
    return c == 0xD5 || c == 0xA8 || c == 0xD3 || c == 0x8D || c == 0x20 || c == 0xDA || c == 0x81 ||
           c == 0xD9 || c == 0xDB || c == 0xAD;
}

static void cmd(panel_t *p, uint8_t c, int arg)
{
    if (c >= 0xB0 && c <= 0xB7)
        p->page = c & 7u;
    else if (c <= 0x0F)
        p->col = (p->col & 0xF0u) | c;
    else if (c >= 0x10 && c <= 0x1F)
        p->col = (p->col & 0x0Fu) | (uint32_t)(c & 0x0F) << 4;
    else if (c == 0xAF) {
        p->on = 1;
        p->ons++;
    } else if (c == 0xAE) {
        p->on = 0;
        p->offs++;
    } else if (c == 0xA0 || c == 0xA1)
        p->remap = c;
    else if (c == 0xC0 || c == 0xC8)
        p->scan = c;
    else if (c == 0x81)
        p->contrast = arg;
    else if (c == 0x8D)
        p->pump = arg;
    else if (c == 0xAD)
        p->dcdc = arg;
    else if (c == 0x20)
        p->page_mode = arg;
    else if (c == 0xA8)
        p->mux = arg;
}

static void data(panel_t *p, uint8_t d)
{
    if (!p->on)
        p->drawn_before_on++;
    if (p->col < 132)
        p->ram[p->page][p->col] = d;
    p->col++;
}

int hal_i2c_write(uint8_t a, const uint8_t *b, uint32_t n)
{
    panel_t *p = panel_at(a);
    if (!p) {
        bad_addr++;
        return -1;
    }
    p->writes++;
    if ((int)n + 1 > p->max_len)
        p->max_len = (int)n + 1;
    int any_data = 0;
    uint32_t k = 0;
    while (k < n) {
        uint8_t ctl = b[k++];
        int is_data = (ctl & 0x40) != 0, more = (ctl & 0x80) != 0;
        if (more) {                     /* Co = 1: one byte, then another control byte */
            if (k >= n)
                break;
            uint8_t x = b[k++];
            if (is_data) {
                data(p, x);
                any_data = 1;
            } else {
                cmd(p, x, -1);
            }
            continue;
        }
        while (k < n) {                 /* Co = 0: the rest */
            uint8_t x = b[k++];
            if (is_data) {
                data(p, x);
                any_data = 1;
            } else {
                int arg = -1;
                if (cmd_has_arg(x) && k < n)
                    arg = b[k++];
                cmd(p, x, arg);
            }
        }
    }
    p->data_writes += any_data;
    return 0;
}

int hal_i2c_read(uint8_t a, uint8_t *b, uint32_t n) { (void)a; (void)b; (void)n; return -1; }

static uint8_t out[4096];
static int out_n;
void hal_midi_send_cable(uint8_t cable, const uint8_t *m, uint32_t n)
{
    (void)cable;
    if (out_n + (int)n < (int)sizeof out) {
        memcpy(out + out_n, m, n);
        out_n += (int)n;
    }
}
void hal_midi_send(const uint8_t *m, uint32_t n) { hal_midi_send_cable(0, m, n); }
void hal_midi_poll(void) {}
void hal_midi_flush(uint32_t t) { (void)t; }
void hal_uid(uint8_t uid[12]) { for (int i = 0; i < 12; i++) uid[i] = (uint8_t)(0x10 + i); }
void hal_reboot(int to_bootloader) { (void)to_bootloader; for (;;) {} }
const hal_fw_ops_t *hal_fw_ops(void) { return 0; }

/* ---- helpers ---------------------------------------------------------- */

static int most_data_writes_a_pass;

static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        for (uint32_t t = 0; t < 1000u / gm_device.scan_us; t++)
            gm_scan_tick();
        now_ms++;
        int before = panel[0].data_writes + panel[1].data_writes;
        gm_poll();
        int d = panel[0].data_writes + panel[1].data_writes - before;
        if (d > most_data_writes_a_pass)
            most_data_writes_a_pass = d;
    }
}

/* Pixel (x, y) of the panel's picture: RAM column x + off. */
static int pixel(const panel_t *p, uint32_t off, uint32_t x, uint32_t y)
{
    return (p->ram[y / 8u][x + off] >> (y % 8u)) & 1u;
}

/* How many pixels of text s (padded with blanks to n) drawn at text line
 * `line`, pixel x0, scale sc differ from the panel's picture. */
static int text_diff(const panel_t *p, uint32_t off, uint32_t line, uint32_t x0, uint32_t sc, const char *s, uint32_t n)
{
    int bad = 0;
    size_t len = strlen(s);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *g = gm_text_glyph(i < len ? s[i] : ' ');
        for (uint32_t k = 0; k < 6u; k++) {
            uint8_t col = k < 5u ? g[k] : 0u;
            for (uint32_t r = 0; r < sc; r++)
                for (uint32_t b = 0; b < 8u; b++)
                    for (uint32_t q = 0; q < sc; q++) {
                        uint32_t x = x0 + (i * 6u + k) * sc + r, y = line * 8u + b * sc + q;
                        if (pixel(p, off, x, y) != (int)((col >> b) & 1u))
                            bad++;
                    }
        }
    }
    return bad;
}

/* Lit pixels outside the given rows (8-pixel lines lo..hi-1). */
static int lit_outside(const panel_t *p, uint32_t off, uint32_t lo, uint32_t hi)
{
    int n = 0;
    for (uint32_t pg = 0; pg < 8u; pg++)
        if (pg < lo || pg >= hi)
            for (uint32_t x = 0; x < 128u; x++)
                n += p->ram[pg][x + off] != 0;
    return n;
}

/* The last CC 7 value sent, or -1. */
static int last_cc7(void)
{
    int v = -1;
    for (int i = 0; i + 2 < out_n; i++) {
        if (out[i] == 0xF0) {
            while (i < out_n && out[i] != 0xF7)
                i++;
            continue;
        }
        if ((out[i] & 0xF0) == 0xB0 && out[i + 1] == 7)
            v = out[i + 2];
    }
    return v;
}

int main(void)
{
    panel_t *big = &panel[0], *sm = &panel[1];
    gm_init(0x01000000u, 0);
    CHECK(bad_addr == 0, "only 0x3C and 0x3D written (%d other writes)", bad_addr);
    CHECK(big->offs >= 1 && big->ons == 1 && big->on, "SSD1306: display off, set up, then on (%d off, %d on)",
          big->offs, big->ons);
    CHECK(big->drawn_before_on > 0, "SSD1306: its RAM written (cleared) before the display goes on");
    CHECK(big->pump == 0x14 && big->page_mode == 0x02 && big->mux == 63 && big->remap == 0xA1 && big->scan == 0xC8,
          "SSD1306: charge pump on, page mode, 64 rows, not turned (%02X %02X %d %02X %02X)", big->pump,
          big->page_mode, big->mux, big->remap, big->scan);
    CHECK(big->contrast == 0xCF, "SSD1306: contrast 0xCF by default (%d)", big->contrast);
    CHECK(sm->dcdc == 0x8B && sm->pump == 0 && sm->page_mode == 0 && sm->remap == 0xA0 && sm->scan == 0xC0,
          "SH1106: DC-DC on, no SSD1306 commands, turned (%02X %02X %02X %02X %02X)", sm->dcdc, sm->pump,
          sm->page_mode, sm->remap, sm->scan);
    CHECK(sm->contrast == 64 && sm->on, "SH1106: contrast 64, on (%d)", sm->contrast);

    run_ms(100);
    CHECK(text_diff(big, 0, 1, 0, 2, "gemifi", 10) == 0, "big: \"gemifi\" at boot, at scale 2 (%d pixels off)",
          text_diff(big, 0, 1, 0, 2, "gemifi", 10));
    CHECK(text_diff(big, 0, 6, 4, 1, "CH     1", 8) == 0, "chan: \"CH     1\" from x 4 (%d pixels off)",
          text_diff(big, 0, 6, 4, 1, "CH     1", 8));
    CHECK(lit_outside(big, 0, 1, 7) == 0, "nothing lit outside lines 1..6 (%d bytes)", lit_outside(big, 0, 1, 7));
    CHECK(lit_outside(sm, 2, 0, 0) == 0, "small: blank (shows none)");
    CHECK(big->max_len <= 128 && sm->max_len <= 128, "every write fits the ESP Wire buffer (%d, %d bytes)",
          big->max_len, sm->max_len);
    CHECK(most_data_writes_a_pass == 1, "one run a main-loop pass after boot (%d)", most_data_writes_a_pass);

    /* Nothing changes: nothing on the bus. */
    int w0 = big->writes, w1 = sm->writes;
    run_ms(200);
    CHECK(big->writes == w0 && sm->writes == w1, "no change, no writes (%d, %d)", big->writes - w0, sm->writes - w1);

    /* The pot: its CC on the big display, as "7" and the value right-aligned. */
    out_n = 0;
    adc = 2048;
    run_ms(200);
    int v = last_cc7();
    CHECK(v > 0, "the pot sent CC 7 (%d)", v);
    char want[16];
    snprintf(want, sizeof want, "7%*d", 9, v);
    CHECK(text_diff(big, 0, 1, 0, 2, want, 10) == 0, "big: \"%s\" (%d pixels off)", want,
          text_diff(big, 0, 1, 0, 2, want, 10));
    CHECK(text_diff(big, 0, 6, 4, 1, "CH     1", 8) == 0, "chan unchanged");

    /* A message: a '.' takes a place on a text display, and lower case has its glyphs. */
    gm_display_message("Ab.c");
    run_ms(50);
    CHECK(text_diff(big, 0, 1, 0, 2, "Ab.c", 10) == 0, "big: \"Ab.c\", 4 places (%d pixels off)",
          text_diff(big, 0, 1, 0, 2, "Ab.c", 10));

    /* A host's text on the SH1106: 2 columns in, in its RAM. */
    const uint8_t hello[] = "Hello, gemifi!";
    gm_host_on_text(2, 0, 0, hello, sizeof hello - 1);
    run_ms(50);
    CHECK(text_diff(sm, 2, 0, 0, 1, "Hello, gemifi!", 21) == 0, "small: the host's text, from RAM column 2 (%d off)",
          text_diff(sm, 2, 0, 0, 1, "Hello, gemifi!", 21));
    int lit01 = 0;
    for (uint32_t pg = 0; pg < 8u; pg++)
        lit01 += sm->ram[pg][0] != 0 || sm->ram[pg][1] != 0 || sm->ram[pg][130] != 0 || sm->ram[pg][131] != 0;
    CHECK(lit01 == 0, "small: RAM columns 0, 1, 130 and 131 (off the panel) untouched");
    gm_host_on_text(2, 0, 7, (const uint8_t *)"G", 1);
    run_ms(50);
    CHECK(text_diff(sm, 2, 0, 0, 1, "Hello, Gemifi!", 21) == 0, "small: one character changed");
    gm_display_host_release();
    run_ms(50);
    CHECK(lit_outside(sm, 2, 0, 0) == 0, "small: blank again after FB_RELEASE");
    CHECK(most_data_writes_a_pass == 1, "still one run a pass (%d)", most_data_writes_a_pass);

    printf("test_oled: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
