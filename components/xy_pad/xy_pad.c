/*
 * xy_pad.c: the xy_pad hub type, a 4-wire resistive XY pad read in software
 * (components/xy_pad/README.md). A hub type on the hub base
 * (components/hub/gm_hub.h): two analog channels, x and y. Its state (u.xy)
 * stays in the core: vpin_analog_jump (gm_vpin.h) reads it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

/* One plate is driven at a time, + pin high and - pin low; the other plate
 * floats and takes the voltage where it's pressed, read on its sense pin
 * (gm_hub_xy_t sense[]). Untouched, the floating plate reads
 * outside touch_min..touch_max (stock Code 61: 592 - 22 .. 3408 + 22). The
 * other plate is let go before this one is driven, so a touch never joins
 * two driven pins. */
static void xy_drive(const gm_hub_t *h, uint32_t axis)
{
    const gm_pin_t *drv = &h->xy.drive[axis ? 2 : 0], *oth = &h->xy.drive[axis ? 0 : 2];
    gm_pin_t sense = h->xy.sense[axis];
    for (uint32_t k = 0; k < 2; k++)
        if (oth[k].hub == 0)
            hal_pin_mode(oth[k].num, oth[k].num == sense.num ? HAL_ANALOG : HAL_IN);
    gm_hub_mcu_out(drv[0], 1);
    gm_hub_mcu_out(drv[1], 0);
}

static void xy_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    s->u.xy.raw[0] = s->u.xy.raw[1] = 0xFFFFu;
    xy_drive(h, 0);
    s->channel = 0;
    s->u.xy.t = hal_millis();
}

/* A touch counts after XY_SETTLE readings in a row inside the window
 * (each axis twice, ~4 ms): the first as a finger lands can be anywhere. */
#define XY_SETTLE 4u

/* Once per ms: read the axis driven since the last switch (a whole ms to
 * settle), then drive the other. So each axis is new every 2 ms. -1 while
 * untouched (the input holds its last value). */
static int32_t xy_analog(const gm_hub_t *h, gm_hub_state_t *s, uint32_t ch)
{
    uint32_t now = hal_millis();
    if (now != s->u.xy.t && s->channel >= 0) {
        uint32_t a = (uint32_t)s->channel;
        gm_pin_t sense = h->xy.sense[a];
        int32_t v = sense.hub == 0 ? hal_adc_read(sense.num) : -1;
        s->u.xy.raw[a] = v < 0 ? 0xFFFFu : (uint16_t)v;
        int t = 1;
        for (uint32_t k = 0; k < 2; k++)
            t = t && s->u.xy.raw[k] >= h->xy.touch_min && s->u.xy.raw[k] <= h->xy.touch_max;
        if (!t)
            s->u.xy.touch = 0;
        else if (s->u.xy.touch < XY_SETTLE && ++s->u.xy.touch == XY_SETTLE)
            s->u.xy.fresh = 3u;
        s->channel = (int8_t)(a ^ 1u);
        xy_drive(h, a ^ 1u);
        s->u.xy.t = now;
    }
    if (ch > 1u || s->u.xy.touch < XY_SETTLE)
        return -1;
    return s->u.xy.raw[ch];
}

const gm_hub_ops_t gm_xy_pad_ops = {
    .flags = GM_HUBF_IN_REG, .init = xy_init, .analog = xy_analog,
};
