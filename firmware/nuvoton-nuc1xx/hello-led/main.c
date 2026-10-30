/*
 * hello-led: milestone M1 (bootloader-design.md 10).
 *
 * 48 MHz clocks; the Oct- LED blinks at 2 Hz; the Oct+ LED is lit while the
 * Oct+ button is held. Proves our startup code, clock init and pin map, and
 * the stock LDROM -> APROM handoff. Carries a .gfw header (mkimage) so the
 * gemifi bootloader will accept it later.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sys.h"
#include "board.h"

int main(void)
{
    sys_clock_init();
    board_gpio_init();
    sys_tick_init();

    uint32_t t0 = sys_tick_ms();
    int on = 0;
    for (;;) {
        uint32_t now = sys_tick_ms();
        if (now - t0 >= 250) {
            t0 = now;
            on = !on;
            board_led(LED_OCT_DOWN, on);
        }
        board_led(LED_OCT_UP, (board_buttons() >> BTN_OCT_UP) & 1u);
    }
}
