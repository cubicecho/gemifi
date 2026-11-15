# max7219

An LED driver platform ([led_driver](../led_driver/README.md), a hub type on the [hub](../hub/README.md) base): `max7219.c` (the driver), `max7219.h` (its config), `__init__.py` (the YAML: validation and generated C). Its host test is `core/test/test_seg_hubs.c`, with the TM1637's.

`max7219` (also the MAX7221; ESPHome's keys) drives a chain of `num_chips` chips on one `din_pin` / `clk_pin` / `cs_pin`. Chip 0 is the one whose DIN is the MCU's. Pin = chip × 64 + digit × 8 + segment, with segment 0–6 = A–G and 7 = DP, and digit 0 = DIG0, so a `segment7` character is its digit's first pin (`number: 0`, `8`, `16`, ...) and every chip is also 64 single LEDs. The chips' own decode is off: the core sends segments. The first write sets the chain up (display test off, no decode, scan limit `scan_limit - 1`, intensity) and writes every digit before turning it on (shutdown off), so nothing random shows. After that, a digit that changed in any chip is one frame: that digit register of every chip, the last chip's word first, taken on CS rising. Words go MSB first, DIN set while CLK is low and taken on the rising edge, each phase ≥ 100 ns on `hal_cycles()` (`MX_PHASE_NS`; 1 µs a phase on a platform without a cycle counter, the ESP and NUC builds). A frame counts 2 a chip against the budget, and at least one goes out per pass. Several chains can share `clk_pin` and `din_pin` (`allow_other_uses: true` on each), each with its own `cs_pin`. A low `scan_limit` lights each digit longer: the datasheet's "Scan-Limit Register" says how to choose RSET for fewer than 4.

One of the LED drivers for segment digits written from the main loop ([led_driver](../led_driver/README.md)): its pins are kept in the hub RAM, and `send` writes only what changed, within the shared 4-byte budget, never on the LED update's pass. Untested on hardware.

```yaml
led_driver:
  - { id: digits, platform: max7219, clk_pin: PA5, din_pin: PA7, cs_pin: PA4, intensity: 8, scan_limit: 3 }
displays:
  - { id: preset_display, type: segment7, shows: preset_number,
      chars: [{ led_driver: digits, number: 0 }, { led_driver: digits, number: 8 }, { led_driver: digits, number: 16 }] }
```
