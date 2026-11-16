/*
 * st7567.h: the st7567 display platform's config (gm_screen_t .st7567), an
 * external component (core/README.md "External components").
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_ST7567_H
#define GEMIFI_ST7567_H

#include <stdint.h>

typedef struct {
    uint8_t addr;         /* 7-bit I2C address */
    uint8_t bus;          /* the i2c: bus      */
    uint8_t contrast;     /* 0..63 (electronic volume) */
} gm_screen_st7567_t;

#endif
