# sn74hc595

A hub type ([hub](../hub/README.md)): a chain of 74HC595 serial-in shift registers, 8 outputs a chip on three MCU pins, for LEDs and key-matrix rows.

```yaml
sn74hc595:
  - { id: leds, data_pin: PB15, clock_pin: PB13, latch_pin: PA8, sr_count: 2 }
leds:
  - { id: fs_1_led, pin: { sn74hc595: leds, number: 0 } }
```

<!-- keys -->

Hub types and the pins they give: [hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components).

Pins 0–7 are the outputs of the chip on `data_pin`, 8–15 those of the next. They are written once per scan, from the scan tick, and only when one changed: the highest pin first, so the first bit shifted ends up in the last chip, then a latch pulse. So a whole key matrix can live on shift registers.

## Files

- `sn74hc595.c`: the driver.
- `__init__.py`: the YAML: a `ShiftRegister` of the hub base. Its config (`.sr`, `include/gm_device.h`) is shared with the `sn74hc165`, and its pins are the cached `out` word.
- Its host test is `core/test/test_leds.c` (`test_leds_many`: LEDs on four chains).
