/*
 * gm_screen.h: the display base component (components/display/README.md).
 *
 * A screen is a panel the YAML's display: list declares (an SSD1306 OLED
 * ...), which text displays (gm_display.h GM_DISPLAY_TEXT, displays:)
 * draw on. This base keeps each screen's framebuffer: width x height / 8
 * bytes, a byte = 8 pixels down (bit 0 at the top) at page * width + x,
 * and which pages changed. Each platform (components/<platform>/) only
 * sends what changed to its panel, from the main loop, within the bus
 * budget the hubs leave (gm_screen_send).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_SCREEN_H
#define GEMIFI_GM_SCREEN_H

#include <stdint.h>
#include "gm_components.h"      /* GM_SCREEN_CONFIG_MEMBERS, GM_SCREEN_OPS ... */

#ifdef __cplusplus
extern "C" {
#endif

enum {                          /* gm_screen_t.type: the platform */
    GM_SCREEN_SSD1306 = 1,      /* SSD1306 / SH1106 I2C OLED (.ssd1306) */
    GM_SCREEN_N_TYPES
};

/* A screen: its platform, its size and the platform's own config. */
typedef struct {
    uint8_t type;               /* GM_SCREEN_*                          */
    uint8_t width;              /* pixels                               */
    uint8_t height;             /* pixels, a multiple of 8 (a page)     */
    union {
        uint8_t none;
        GM_SCREEN_CONFIG_MEMBERS    /* each platform's (gm_components.h) */
    };
} gm_screen_t;

/* A screen's state, kept by the base. */
typedef struct {
    uint16_t fb;                /* where its framebuffer starts in the screen RAM */
    uint8_t pages;              /* bit = page changed since it was sent  */
    uint8_t never;              /* the panel isn't set up yet            */
} gm_screen_state_t;

typedef struct {
    /* Main loop: send what changed (fb against sent, in the pages marked
     * in s->pages, which it clears as they go out), at most budget bus
     * bytes; at least one write when something changed. With s->never:
     * set the panel up and send all of it, whatever the budget, then clear
     * never. Returns the budget left. */
    uint32_t (*send)(const gm_screen_t *c, gm_screen_state_t *s, const uint8_t *fb, uint8_t *sent,
                     uint32_t budget);
} gm_screen_ops_t;

extern const gm_screen_t gm_screens[];  /* the device's (generated config): GM_N_SCREENS */

/* Start-up: every framebuffer blank, every panel to be set up. */
void gm_screen_init(void);

/* Draw n columns of 8 pixels (bit 0 at the top) on screen i from byte at
 * (page * width + x), each pixel scale x scale (1..4: scale pages tall).
 * What falls off the screen is dropped; changed pages are marked. */
void gm_screen_draw(uint32_t i, uint32_t at, const uint8_t *cols, uint32_t n, uint32_t scale);

/* Main loop: each screen's changes, sharing budget bus bytes; returns
 * what's left. ~0: everything at once. */
uint32_t gm_screen_send(uint32_t budget);

#ifdef __cplusplus
}
#endif

#endif
