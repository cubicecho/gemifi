/*
 * mcp23017.c: the mcp23017 hub type, an MCP23017 16-pin I/O expander on the
 * I2C bus, read and written from the main loop
 * (components/mcp23017/README.md). A hub type on the hub base
 * (components/hub/gm_hub.h): its config is the I2C expanders' .i2c, and
 * the base's gm_hub_i2c_* keep its pins' directions and pull-ups.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

#define MCP_IODIRA 0x00
#define MCP_GPPUA  0x0C
#define MCP_GPIOA  0x12
#define MCP_OLATA  0x14

static void mcp_write16(uint8_t addr, uint8_t reg, uint32_t v)
{
    uint8_t b[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    hal_i2c_write(addr, b, 3);
}

/* A chip nothing reads (only LEDs on it) isn't read: an MCP23017 read is
 * two transactions, ~120 us at 400 kHz. */
static void mcp23017_refresh(const gm_hub_t *h, gm_hub_state_t *s)
{
    uint8_t b[2], reg = MCP_GPIOA;
    if (s->u.io.in_used && hal_i2c_write(h->i2c.addr, &reg, 1) == 0 && hal_i2c_read(h->i2c.addr, b, 2) == 0)
        s->in = b[0] | (uint32_t)b[1] << 8;
}

static void mcp23017_flush(const gm_hub_t *h, gm_hub_state_t *s)
{
    if (s->u.io.cfg_dirty) {
        mcp_write16(h->i2c.addr, MCP_IODIRA, ~s->u.io.dir_out & 0xFFFFu);
        mcp_write16(h->i2c.addr, MCP_GPPUA, s->u.io.pullup & 0xFFFFu);
        s->u.io.cfg_dirty = 0;
        s->never = 1;
    }
    if (s->out != s->written || s->never) {
        mcp_write16(h->i2c.addr, MCP_OLATA, s->out);
        s->written = s->out;
        s->never = 0;
    }
}

const gm_hub_ops_t gm_mcp23017_ops = {
    .flags = GM_HUBF_BUS | GM_HUBF_IN_REG, .init = gm_hub_i2c_init, .input = gm_hub_i2c_input,
    .output = gm_hub_i2c_output, .refresh = mcp23017_refresh, .flush = mcp23017_flush,
};
