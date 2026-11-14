/*
 * xy_pad.h: the xy_pad hub type's config (gm_hub_t .xy);
 * components/xy_pad/README.md. gm_device.h includes it through the
 * generated gm_components.h. Its state (u.xy) is the core's (gm_hub.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_XY_PAD_H
#define GEMIFI_XY_PAD_H

#include <stdint.h>
#include "gm_pin.h"

/* .xy: a 4-wire resistive XY pad: analog only, channel 0 = X, 1 = Y. Read in
 * software (xy_pad.c), one axis per ms in turn. A touch: both readings within
 * touch_min..touch_max. */
typedef struct {
    gm_pin_t drive[4];    /* X+, X-, Y+, Y- (MCU pins)                     */
    gm_pin_t sense[2];    /* the pins X and Y are read on: a Y-plate and an
                             X-plate pin (ADC pins)                        */
    uint16_t touch_min, touch_max;
} gm_hub_xy_t;

#endif
