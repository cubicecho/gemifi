/*
 * ht16k33.c: the ht16k33 LED driver platform, an HT16K33 on the I2C bus,
 * written from the main loop (components/ht16k33/README.md). A hub type on
 * the led_driver base (components/led_driver): its pins are bits in the hub
 * RAM (gm_led_driver_*), and send writes what changed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"
#include "led_driver.h"

/* HT16K33 (I2C): 16 bytes of display RAM, COM c = bytes 2c (ROW0-7) and
 * 2c + 1 (ROW8-15). The first write: 0x21 (oscillator on), the RAM from
 * address 0, 0xE0 | brightness, 0x81 (display on, no blink). After that,
 * the first run of changed bytes as one write (its address, its bytes), as
 * many as the budget has room for (an I2C byte at 400 kHz, ~23 us, counts
 * 2), at least one. The writes aren't checked: a missing chip costs its
 * bytes once per change, as the MCP23017's do. */
#define HT_RAM 16u
#define HT_COST 2u

static void ht_cmd(const gm_hub_t *h, uint8_t c)
{
    hal_i2c_write(h->ht.bus, h->ht.addr, &c, 1);
}

static uint32_t ht16k33_send(const gm_hub_t *h, gm_hub_state_t *s, uint32_t budget)
{
    if (!s->u.led_driver.dirty || s->ram + HT_RAM > GM_HUB_RAM)
        return budget;
    uint8_t *ram = &gm_hub_ram[s->ram], *sent = &gm_hub_ram_sent[s->ram], b[HT_RAM + 1];
    uint32_t k = 0, all = s->never, used = 0;
    if (all)
        ht_cmd(h, 0x21u);
    else
        while (k < HT_RAM && ram[k] == sent[k])
            k++;
    if (k < HT_RAM) {
        uint32_t n = 0;
        b[n++] = (uint8_t)k;
        used = 2u * HT_COST;                /* the address byte and the RAM address */
        do {
            b[n++] = ram[k];
            sent[k] = ram[k];
            k++;
            used += HT_COST;
        } while (k < HT_RAM && (all || (ram[k] != sent[k] && used + HT_COST <= budget)));
        hal_i2c_write(h->ht.bus, h->ht.addr, b, n);
    }
    if (all) {
        ht_cmd(h, (uint8_t)(0xE0u | (h->ht.brightness & 15u)));
        ht_cmd(h, 0x81u);
        s->never = 0;
    }
    while (k < HT_RAM && ram[k] == sent[k])
        k++;
    s->u.led_driver.dirty = k < HT_RAM;
    return budget > used ? budget - used : 0u;
}

static void ht16k33_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    (void)h;
    gm_led_driver_init(s, HT_RAM);
}

static void ht16k33_write(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, int level)
{
    (void)h;
    gm_led_driver_write(s, HT_RAM, num, level);
}

const gm_hub_ops_t gm_ht16k33_ops = {
    .flags = GM_HUBF_NO_READ, .init = ht16k33_init, .write = ht16k33_write, .send = ht16k33_send,
};
