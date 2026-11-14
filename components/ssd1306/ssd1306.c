/*
 * ssd1306.c: the ssd1306 display platform: SSD1306 and SH1106 I2C OLEDs
 * (components/ssd1306/README.md), on the display base (components/display/
 * gm_screen.h), which keeps the framebuffer.
 *
 * The first write: the set-up commands (display off), every page, then
 * display on. After that, runs of changed bytes, page by page: one write
 * each, its page and column (each command after a 0x80 control byte), then
 * 0x40 and up to OLED_RUN data bytes (a write fits the ESP Wire buffer, 128
 * bytes). A run is ~24 bus bytes (~0.55 ms at 400 kHz): at least one goes
 * out a pass, more while the budget lasts (a bus byte counts OLED_COST, as
 * an HT16K33's); gm_screen_send(~0) sends everything. The SH1106's RAM is
 * 132 columns, the panel's 128 from column 2. The writes aren't checked, as
 * the I2C hubs' aren't.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_hal.h"
#include "gm_screen.h"

#define OLED_RUN 16u
#define OLED_HEAD 7u                /* page, column low, column high (3 x 2), 0x40 */
#define OLED_COST 2u

static void oled_setup(const gm_screen_t *c)
{
    const gm_screen_ssd1306_t *o = &c->ssd1306;
    int sh = (o->flags & GM_OLED_SH1106) != 0, rot = (o->flags & GM_OLED_ROTATE) != 0;
    uint8_t b[32];
    uint32_t n = 0;
    b[n++] = 0x00;                          /* control: commands follow */
    b[n++] = 0xAE;                          /* display off */
    b[n++] = 0xD5; b[n++] = 0x80;           /* clock divide, oscillator */
    b[n++] = 0xA8; b[n++] = (uint8_t)(c->height - 1u);   /* multiplex ratio */
    b[n++] = 0xD3; b[n++] = 0x00;           /* display offset */
    b[n++] = 0x40;                          /* start line 0 */
    if (sh) {
        b[n++] = 0xAD; b[n++] = 0x8B;       /* SH1106: DC-DC on */
    } else {
        b[n++] = 0x8D; b[n++] = 0x14;       /* SSD1306: charge pump on */
        b[n++] = 0x20; b[n++] = 0x02;       /* page addressing (as the SH1106 has) */
    }
    b[n++] = rot ? 0xA0 : 0xA1;             /* segment remap */
    b[n++] = rot ? 0xC0 : 0xC8;             /* COM scan direction */
    b[n++] = 0xDA; b[n++] = c->height > 32u ? 0x12 : 0x02;   /* COM pins */
    b[n++] = 0x81; b[n++] = o->contrast;
    b[n++] = 0xD9; b[n++] = sh ? 0x22 : 0xF1;   /* pre-charge */
    b[n++] = 0xDB; b[n++] = sh ? 0x35 : 0x40;   /* VCOMH */
    b[n++] = 0xA4;                          /* show the RAM */
    b[n++] = 0xA6;                          /* not inverted */
    hal_i2c_write(o->addr, b, n);
}

/* n (<= OLED_RUN) framebuffer bytes from column x of page pg. */
static void oled_write(const gm_screen_t *c, uint32_t pg, uint32_t x, const uint8_t *d, uint32_t n)
{
    uint8_t b[OLED_HEAD + OLED_RUN];
    uint32_t col = x + ((c->ssd1306.flags & GM_OLED_SH1106) ? 2u : 0u), k = 0;
    b[k++] = 0x80; b[k++] = (uint8_t)(0xB0u | pg);
    b[k++] = 0x80; b[k++] = (uint8_t)(col & 0x0Fu);
    b[k++] = 0x80; b[k++] = (uint8_t)(0x10u | (col >> 4));
    b[k++] = 0x40;                          /* data follows */
    for (uint32_t i = 0; i < n; i++)
        b[k++] = d[i];
    hal_i2c_write(c->ssd1306.addr, b, k);
}

static uint32_t ssd1306_send(const gm_screen_t *c, gm_screen_state_t *s, const uint8_t *fb, uint8_t *sent,
                             uint32_t budget)
{
    uint32_t w = c->width, np = c->height / 8u;
    uint32_t all = s->never, used = 0;
    if (all)
        oled_setup(c);
    for (uint32_t pg = 0; pg < np; pg++) {
        if (!(s->pages & (1u << pg)))
            continue;
        const uint8_t *r = fb + pg * w;
        uint8_t *t = sent + pg * w;
        uint32_t x = 0;
        for (;;) {
            if (!all)
                while (x < w && r[x] == t[x])
                    x++;
            if (x >= w) {
                s->pages = (uint8_t)(s->pages & ~(1u << pg));
                break;
            }
            if (!all && used && used >= budget)
                return 0;                   /* the rest on the next passes */
            uint32_t end = x + OLED_RUN < w ? x + OLED_RUN : w;
            if (!all)
                while (end > x + 1u && r[end - 1u] == t[end - 1u])
                    end--;
            oled_write(c, pg, x, r + x, end - x);
            for (uint32_t i = x; i < end; i++)
                t[i] = r[i];
            used += (OLED_HEAD + 1u + end - x) * OLED_COST;
            x = end;
        }
    }
    if (all) {
        uint8_t on[2] = { 0x00, 0xAF };     /* display on, the RAM now clear (or drawn) */
        hal_i2c_write(c->ssd1306.addr, on, 2);
        s->never = 0;
    }
    return budget > used ? budget - used : 0u;
}

const gm_screen_ops_t gm_ssd1306_ops = { .send = ssd1306_send };
