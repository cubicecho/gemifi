/*
 * sn74hc165.c: the sn74hc165 hub type, a chain of 74HC165 parallel-in
 * shift registers: inputs, read into the cached in word on every scan tick
 * (components/sn74hc165/README.md). A hub type on the hub base
 * (components/hub/gm_hub.h): its config is the shift registers' .sr.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

static void hc165_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    (void)s;
    gm_hub_mcu_out(h->sr.clock, 0);
    gm_hub_mcu_out(h->sr.latch, 1);
    if (h->sr.data.hub == 0)
        hal_pin_mode(h->sr.data.num, HAL_IN);
}

static void hc165_refresh(const gm_hub_t *h, gm_hub_state_t *s)
{
    /* Pin numbers as ESPHome: 0-7 on the chip wired to the MCU, 0 = its A.
     * SH/LD low loads the parallel inputs; then QH = first chip's H
     * (pin 7), and each clock shifts the next bit in. */
    hal_pin_write(h->sr.latch.num, 0);
    hal_delay_us(1);
    hal_pin_write(h->sr.latch.num, 1);
    uint32_t bits = 8u * (h->sr.chain ? h->sr.chain : 1u), v = 0;
    if (bits > 32)
        bits = 32;
    for (uint32_t b = 0; b < bits; b++) {
        uint32_t pin = (b / 8u) * 8u + 7u - (b % 8u);
        if (hal_pin_read(h->sr.data.num))
            v |= 1u << pin;
        gm_hub_pulse(h->sr.clock.num);
    }
    s->in = v;
}

const gm_hub_ops_t gm_sn74hc165_ops = {
    .flags = GM_HUBF_IN_REG, .init = hc165_init, .refresh = hc165_refresh,
};
