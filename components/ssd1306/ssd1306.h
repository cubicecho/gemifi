/*
 * ssd1306.h: the ssd1306 display platform's config (gm_screen_t .ssd1306):
 * an SSD1306 or SH1106 OLED on the I2C bus (components/ssd1306/README.md).
 * gm_screen.h includes it through the generated gm_components.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_SSD1306_H
#define GEMIFI_SSD1306_H

#include <stdint.h>

#define GM_OLED_SH1106  (1u << 0)   /* SH1106: 132-column RAM, 2 columns in, page addressing only */
#define GM_OLED_ROTATE  (1u << 1)   /* turned 180 degrees (segment remap and COM scan reversed)   */

typedef struct {
    uint8_t addr;         /* 7-bit I2C address, 0x3C or 0x3D               */
    uint8_t contrast;     /* 0..255 (command 0x81)                         */
    uint8_t flags;        /* GM_OLED_*                                     */
} gm_screen_ssd1306_t;

#endif
