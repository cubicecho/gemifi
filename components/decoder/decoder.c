/*
 * decoder.c: the decoder hub type, 74HC138-style one-of-N decoders driving
 * matrix rows (components/decoder/README.md). A hub type on the hub base
 * (components/hub/gm_hub.h): this sets up its address and enable lines.
 * Its config (.dec) stays in the core, with the scan's path: vpin_write
 * selects an output itself, with a precomputed store where it can
 * (core/src/vpin.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hub.h"

static void decoder_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    (void)s;
    for (uint32_t k = 0; k < h->dec.n_addr && k < 4; k++)
        gm_hub_mcu_out(h->dec.addr[k], 0);
    for (uint32_t k = 0; k < h->dec.n_en && k < 3; k++)
        gm_hub_mcu_out(h->dec.en[k], 0);
}

const gm_hub_ops_t gm_decoder_ops = {
    .flags = GM_HUBF_SELECT, .init = decoder_init,  /* writes: vpin_write */
};
