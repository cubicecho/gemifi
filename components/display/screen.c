/*
 * screen.c: the display base component: the screens' framebuffers
 * (gm_screen.h; components/display/README.md). The platforms send them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_screen.h"

GM_SCREEN_OPS_EXTERNS               /* each platform's (gm_components.h) */

static const gm_screen_ops_t *const screen_ops[GM_SCREEN_N_TYPES] = {
    GM_SCREEN_OPS
};

static uint8_t screen_ram[GM_SCREEN_RAM];
static uint8_t screen_sent[GM_SCREEN_RAM];  /* what each panel has */
static gm_screen_state_t st[GM_N_SCREENS];

static uint32_t fb_bytes(const gm_screen_t *c)
{
    return (uint32_t)c->width * (c->height / 8u);
}

void gm_screen_init(void)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i < GM_N_SCREENS; i++) {
        const gm_screen_t *c = &gm_screens[i];
        uint32_t n = fb_bytes(c);
        st[i].fb = (uint16_t)at;
        st[i].pages = (uint8_t)((1u << (c->height / 8u)) - 1u);
        st[i].never = 1;
        for (uint32_t k = 0; k < n && at + k < GM_SCREEN_RAM; k++)
            screen_ram[at + k] = 0;
        at += n;
    }
}

void gm_screen_draw(uint32_t i, uint32_t at, const uint8_t *cols, uint32_t n, uint32_t scale)
{
    if (i >= GM_N_SCREENS)
        return;
    const gm_screen_t *c = &gm_screens[i];
    gm_screen_state_t *s = &st[i];
    uint32_t w = c->width, np = c->height / 8u;
    if (s->fb + fb_bytes(c) > GM_SCREEN_RAM)
        return;
    uint8_t *fb = &screen_ram[s->fb];
    uint32_t page = at / w, x0 = at % w;
    scale = scale < 1u ? 1u : scale > 4u ? 4u : scale;
    uint32_t ones = (1u << scale) - 1u;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t v = 0;                     /* the column, each pixel scale tall */
        for (uint32_t b = 0; b < 8u; b++)
            if (cols[k] & (1u << b))
                v |= ones << (b * scale);
        for (uint32_t r = 0; r < scale; r++) {
            uint32_t x = x0 + k * scale + r;
            if (x >= w)
                return;
            for (uint32_t pg = 0; pg < scale && page + pg < np; pg++) {
                uint8_t *p = &fb[(page + pg) * w + x], byte = (uint8_t)(v >> (8u * pg));
                if (*p != byte) {
                    *p = byte;
                    s->pages = (uint8_t)(s->pages | (1u << (page + pg)));
                }
            }
        }
    }
}

uint32_t gm_screen_send(uint32_t budget)
{
    for (uint32_t i = 0; i < GM_N_SCREENS && budget; i++) {
        const gm_screen_t *c = &gm_screens[i];
        const gm_screen_ops_t *o = c->type < GM_SCREEN_N_TYPES ? screen_ops[c->type] : 0;
        gm_screen_state_t *s = &st[i];
        if (!o || !o->send || !s->pages || s->fb + fb_bytes(c) > GM_SCREEN_RAM)
            continue;
        budget = o->send(c, s, &screen_ram[s->fb], &screen_sent[s->fb], budget);
    }
    return budget;
}
