/*
 * cd74hc4067.c: the cd74hc4051 and cd74hc4067 hub types (GM_HUB_MUX), the
 * 74HC4051 / 4067 analog multiplexers: one common pin routed to one of 8 /
 * 16 channels (components/cd74hc4067/README.md). A hub type on the hub base
 * (components/hub/gm_hub.h). Its config (.mux) stays in the core, with the
 * scan's path: vpin_read reads a channel itself, gm_hub_mux_select switches
 * channels (and the muxes sharing select lines), and the hardware analog
 * scan steps them (core/src/vpin.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

static void mcu_in(gm_pin_t p)
{
    if (p.hub == 0)
        hal_pin_mode(p.num, (p.flags & GM_PIN_PULLUP) ? HAL_IN_PULLUP :
                            (p.flags & GM_PIN_PULLDOWN) ? HAL_IN_PULLDOWN : HAL_IN);
}

static void mux_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    (void)s;
    for (uint32_t k = 0; k < h->mux.n_sel && k < 4; k++)
        gm_hub_mcu_out(h->mux.sel[k], 0);
    gm_hub_mcu_out(h->mux.enable, 0);
    mcu_in(h->mux.common);
}

static int32_t mux_analog(const gm_hub_t *h, gm_hub_state_t *s, uint32_t ch)
{
    (void)s;
    if (h->mux.common.hub != 0)
        return -1;
    gm_hub_mux_select(h, (uint32_t)(h - gm_device.hubs), ch);
    return hal_adc_read(h->mux.common.num);
}

const gm_hub_ops_t gm_cd74hc4067_ops = {
    .init = mux_init, .analog = mux_analog,         /* digital reads: vpin_read */
};
