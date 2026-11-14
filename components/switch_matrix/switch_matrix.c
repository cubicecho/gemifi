/*
 * switch_matrix.c: the switch_matrix hub type, a matrix of switches scanned
 * from the scan tick (components/switch_matrix/README.md). A hub type on the
 * hub base (components/hub/gm_hub.h): this sets up its rows and columns.
 * Its config (.sm) and state (u.sm) stay in the core, with the scan's path:
 * vpin_scan_matrices, the hardware scan's vpin_scan_plan / vpin_scan_feed
 * and the button spans (vpin_span_take, vpin_read_span), core/src/vpin.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hub.h"
#include "gm_vpin.h"

static void swmatrix_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    (void)s;
    for (uint32_t c = 0; c < h->sm.n_cols; c++)
        vpin_input(h->sm.cols[c]);
    for (uint32_t r = 0; r < h->sm.n_rows; r++)
        vpin_output(h->sm.rows[r]);
}

const gm_hub_ops_t gm_switch_matrix_ops = {
    .flags = GM_HUBF_IN_REG | GM_HUBF_MATRIX, .init = swmatrix_init,   /* scan: vpin_scan_matrices */
};
