/*
 * pcf8574.c: the pcf8574 hub type, a PCF8574 8-pin quasi-bidirectional I/O
 * expander on the I2C bus, read and written from the main loop
 * (components/pcf8574/README.md). A hub type on the hub base
 * (components/hub/gm_hub.h): its config is the I2C expanders' .i2c, and
 * the base's gm_hub_i2c_* keep its pins' directions.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

static void pcf8574_refresh(const gm_hub_t *h, gm_hub_state_t *s)
{
    uint8_t b;
    if (s->u.io.in_used && hal_i2c_read(h->i2c.addr, &b, 1) == 0)
        s->in = b;
}

static void pcf8574_flush(const gm_hub_t *h, gm_hub_state_t *s)
{
    /* Inputs must stay written high. */
    uint32_t v = (s->out | ~s->u.io.dir_out) & 0xFFu;
    if (v != s->written || s->never) {
        uint8_t b = (uint8_t)v;
        hal_i2c_write(h->i2c.addr, &b, 1);
        s->written = v;
        s->never = 0;
    }
}

const gm_hub_ops_t gm_pcf8574_ops = {
    .flags = GM_HUBF_BUS | GM_HUBF_IN_REG, .init = gm_hub_i2c_init, .input = gm_hub_i2c_input,
    .output = gm_hub_i2c_output, .refresh = pcf8574_refresh, .flush = pcf8574_flush,
};
