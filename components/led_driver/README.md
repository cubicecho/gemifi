# led_driver

The base component of **LED drivers**: chips that light LEDs and segment digits from a RAM of a bit per output, declared ESPHome style under `led_driver:`, each on a platform. A driver still gives pins, as every hub does ([core/README.md "Components"](../../core/README.md#components)), named `{ led_driver: <id>, number: n }`; a segment display's `chars:` name them the same way.

```yaml
led_driver:
  - { id: leds, platform: tm1628, clk_pin: PC10, dio_pin: PC11, stb_pin: PA15 }
  - { id: digits, platform: max7219, clk_pin: PA5, din_pin: PA7, cs_pin: PA4, scan_limit: 3 }
leds:
  - { id: shift_led, pin: { led_driver: leds, number: 37 } }
displays:
  - { id: number, type: segment7, shows: preset_number, chars: [{ led_driver: digits, number: 0 }, { led_driver: digits, number: 8 }, { led_driver: digits, number: 16 }] }
```

| Platform | Component | What |
|---|---|---|
| `tm1628` | [tm1628](../tm1628/README.md) | TM1628-class chips (TM1638, TM1620, ...), bit-banged; `fade:` |
| `max7219` | [max7219](../max7219/README.md) | a chain of MAX7219 / MAX7221, bit-banged |
| `tm1637` | [tm1637](../tm1637/README.md) | a TM1637 (2-wire), bit-banged |
| `ht16k33` | [ht16k33](../ht16k33/README.md) | an HT16K33 on the [I2C bus](../i2c/README.md) (`i2c_id:`, `address:`; ESP only for now) |

Every item takes `id` and `platform`, and its platform's keys. Hubs are numbered in YAML order across all lists, so a driver's place in `led_driver:` is its place among the hubs.

## How it works

- **Pins** are outputs only, a bit each in the hub RAM (`GM_HUB_RAM` counts them): writing one changes the RAM copy and marks the chip dirty if a bit changed. What a pin number means is the platform's: a TM1628's or HT16K33's RAM bit, a MAX7219's chip, digit and segment, a TM1637's grid and segment.
- **Sending** is from the main loop, never the scan interrupt: each chip's `send` writes what changed, within the 4-byte budget the hubs share each pass, never on the LED update's pass. The first send sets the chip up and writes its whole RAM.
- **What the base gives**: the bit pins of the platforms whose state is here (MAX7219, TM1637, HT16K33: `gm_led_driver_init`, `gm_led_driver_write`, `.u.led_driver`), and the weak `hal_ns_to_cycles` the bit-banged buses time their phases with. The TM1628 keeps its config and state in the core (`.tm`, `.u.tm`), because the core's `fade:` reads them ([tm1628](../tm1628/README.md)).

## Files

- `led_driver.h` / `led_driver.c`: `gm_led_driver_state_t`, the bit pins (built when a platform's header defines `GM_LED_DRIVER_BITS`) and the weak `hal_ns_to_cycles`.
- `__init__.py`: `LedDriver`, the `HubType` a platform's builder part derives from (`domain = "led_driver"`, outputs in the hub RAM).

## Adding a driver platform

A component of its own, `components/<platform>/`, as [max7219](../max7219/) does:

1. **`<platform>.h`**: its config struct (a member of `gm_hub_t`'s union). To use the base's bit pins, `#define GM_LED_DRIVER_BITS 1` and include `led_driver.h`.
2. **`<platform>.c`**: its hub ops (`init` with `gm_led_driver_init`, `write` with `gm_led_driver_write`, `send` writing what changed within the budget, clearing `s->u.led_driver.dirty` when all is sent) and a `const gm_hub_ops_t gm_<platform>_ops` with `GM_HUBF_NO_READ`.
3. **`__init__.py`**: a `LedDriver` subclass (`key` = the platform name, `enum`, `num`, `keys` with `"platform"`, `lines`, `old_keys` if it had a list of its own, `schema`, `validate` setting `ram_bytes` and `n_pins`, `range_text`, `c_fields`, `c_header`, `c_config`, `c_ops`, and `c_state = ("gm_led_driver_state_t", "led_driver")` with the bit pins), listed in `HUB_TYPES`, with `DEPENDENCIES = ("led_driver",)`. Add its `GM_HUB_*` value to `core/include/gm_device.h`. Then `gemifi_build.py schema -o site/src/schema/device.json`.
4. **`test/`**: a host test with its YAML, built by `core/test/Makefile` (as `core/test/test_seg_hubs.c`).
