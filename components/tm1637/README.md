# tm1637

An LED driver platform ([led_driver](../led_driver/README.md), a hub type on the [hub](../hub/README.md) base): a TM1637, the 2-wire chip on the usual 4- and 6-digit segment modules, one per `clk_pin` / `dio_pin` pair (there's no chip select, so they can't be shared). Untested on hardware.

```yaml
led_driver:
  - { id: clock, platform: tm1637, clk_pin: PB6, dio_pin: PB7, length: 4 }
displays:
  - { id: text, type: segment7, shows: last_message,
      chars: [{ led_driver: clock, number: 0 }, { led_driver: clock, number: 8 }, { led_driver: clock, number: 16 }, { led_driver: clock, number: 24 }] }
```

<!-- keys -->

Its keys are ESPHome's. One of the LED drivers for segment digits written from the main loop ([led_driver](../led_driver/README.md)): its pins are kept in the hub RAM, and `send` writes only what changed, within the shared 4-byte budget, never on the LED update's pass.

Pin = grid × 8 + segment, bit 0 = SEG1 (a on the usual 4- and 6-digit modules) to bit 7 = SEG8 (the point, or the colon on clock modules). Start (DIO falls while CLK is high), bytes LSB first, each followed by the chip's ACK clock, stop (DIO rises while CLK is high), each phase 3 µs (`TM37_HALF_US`; the chip's clock is slow and modules often have capacitors on both lines), about 60 µs a byte. The ACK isn't read: DIO is driven low through it, the level the chip pulls it to, so a push-pull DIO never fights the chip (`OUTPUT_OPEN_DRAIN` with a pull-up works too). The first write is 0x40 (write, auto-increment), every grid from 0xC0, then 0x88 | `intensity` (on); after that each pass sends the first run of changed grids, 0x40 then 0xC0 | its address and as many bytes as the budget has room for (a byte counts 5), at least one.

## Files

- `tm1637.c`: the driver.
- `tm1637.h`: its config.
- `__init__.py`: the YAML: validation and generated C.
- Its host test is `core/test/test_seg_hubs.c`, with the MAX7219's.
