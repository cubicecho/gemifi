/*
 * led_driver.c: the led_driver base, what its platforms share (led_driver.h;
 * components/led_driver/README.md): the bit pins in the hub RAM (max7219,
 * tm1637, ht16k33) and the bus timing's weak default (tm1628, max7219).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"
#include "led_driver.h"

/* The platform's cycle counter for the bit-banged buses; this weak one
 * says there is none (the drivers then wait 1 us a phase). */
__attribute__((weak)) uint32_t hal_ns_to_cycles(uint32_t ns)
{
    (void)ns;
    return 0;
}

#ifdef GM_LED_DRIVER_BITS
void gm_led_driver_write(gm_hub_state_t *s, uint32_t bytes, uint32_t num, int level)
{
    uint32_t k = num >> 3u;
    if (k >= bytes || s->ram + k >= GM_HUB_RAM)
        return;
    uint8_t *b = &gm_hub_ram[s->ram + k], m = (uint8_t)(1u << (num & 7u));
    uint8_t v = level ? (uint8_t)(*b | m) : (uint8_t)(*b & ~m);
    if (v != *b) {
        *b = v;
        s->u.led_driver.dirty = 1;
    }
}

void gm_led_driver_init(gm_hub_state_t *s, uint32_t n)
{
    s->ram = gm_hub_ram_alloc(n);
    s->u.led_driver.dirty = 1;
    for (uint32_t k = 0; k < n && s->ram + k < GM_HUB_RAM; k++)
        gm_hub_ram[s->ram + k] = 0;
}
#endif
