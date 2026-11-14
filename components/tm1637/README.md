# tm1637

A hub type ([hub](../hub/gm_hub.h), [core/README.md "Virtual pins and hubs"](../../core/README.md#virtual-pins-and-hubs)): `tm1637.c` (the driver), `tm1637.h` (its config), `__init__.py` (the YAML: validation and generated C). Its host test is `core/test/test_seg_hubs.c`, with the MAX7219's.

`tm1637` (ESPHome's keys, `length` = grids) is a 2-wire chip, one per `clk_pin` / `dio_pin` pair (there's no chip select, so they can't be shared). Pin = grid × 8 + segment, bit 0 = SEG1 (a on the usual 4- and 6-digit modules) to bit 7 = SEG8 (the point, or the colon on clock modules). Start (DIO falls while CLK is high), bytes LSB first, each followed by the chip's ACK clock, stop (DIO rises while CLK is high), each phase 3 µs (`TM37_HALF_US`; the chip's clock is slow and modules often have capacitors on both lines), about 60 µs a byte. The ACK isn't read: DIO is driven low through it, the level the chip pulls it to, so a push-pull DIO never fights the chip (`OUTPUT_OPEN_DRAIN` with a pull-up works too). The first write is 0x40 (write, auto-increment), every grid from 0xC0, then 0x88 | `intensity` (on); after that each pass sends the first run of changed grids, 0x40 then 0xC0 | its address and as many bytes as the budget has room for (a byte counts 5), at least one.

One of the LED drivers for segment digits written from the main loop ([MAX7219, TM1637 and HT16K33](../../core/README.md#max7219-tm1637-and-ht16k33)): its pins are kept in the hub RAM, and `send` writes only what changed, within the shared 4-byte budget, never on the LED update's pass. Untested on hardware.

```yaml
tm1637:
  - { id: clock, clk_pin: PB6, dio_pin: PB7, length: 4 }
displays:
  - { id: text, type: segment7, shows: last_message,
      chars: [{ tm1637: clock, number: 0 }, { tm1637: clock, number: 8 }, { tm1637: clock, number: 16 }, { tm1637: clock, number: 24 }] }
```
