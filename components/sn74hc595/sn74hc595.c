/*
 * sn74hc595.c: the sn74hc595 hub type, a chain of 74HC595 serial-in shift
 * registers: outputs, written from the scan tick when they change
 * (components/sn74hc595/README.md). A hub type on the hub base
 * (components/hub/gm_hub.h): its config is the shift registers' .sr, and
 * its pins the cached out word.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

static void hc595_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    (void)s;
    gm_hub_mcu_out(h->sr.data, 0);
    gm_hub_mcu_out(h->sr.clock, 0);
    gm_hub_mcu_out(h->sr.latch, 0);
    gm_hub_mcu_out(h->sr.oe, 0);
}

static void hc595_flush(const gm_hub_t *h, gm_hub_state_t *s)
{
    if (s->out == s->written && !s->never)
        return;
    /* The first bit shifted ends up in the last chip: send the highest
     * pin first. */
    uint32_t bits = 8u * (h->sr.chain ? h->sr.chain : 1u);
    if (bits > 32)
        bits = 32;
    uint32_t v = s->out;
    for (uint32_t b = bits; b-- > 0;) {
        hal_pin_write(h->sr.data.num, (v >> b) & 1u);
        gm_hub_pulse(h->sr.clock.num);
    }
    gm_hub_pulse(h->sr.latch.num);
    s->written = v;
    s->never = 0;
}

const gm_hub_ops_t gm_sn74hc595_ops = {
    .flags = GM_HUBF_IN_REG, .init = hc595_init, .flush = hc595_flush,
};
