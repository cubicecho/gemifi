# led_strip

The base component of **LED strips**: chains of LEDs that show any colour, a byte per colour, declared ESPHome style under `led_strip:`, each on a platform. A strip still gives pins, as every hub does ([core/README.md "Virtual pins and hubs"](../../core/README.md#virtual-pins-and-hubs)), named `{ led_strip: <id>, number: n }`.

```yaml
led_strip:
  - { id: strip, platform: ws2812, pin: GPIO48, count: 16, order: GRB, gamma: 2.2 }
  - { id: pads, platform: maudio_pads, tx_pin: PA2, count: 16 }
leds:
  - { id: l1, pin: { led_strip: strip, number: 0 }, pin2: { led_strip: strip, number: 1 }, pin3: { led_strip: strip, number: 2 } }
```

| Platform | Component | What |
|---|---|---|
| `ws2812` | [ws2812](../ws2812/README.md) | addressable strips on one MCU pin: WS2812B, SK6812 RGB and RGBW (ESP32s and stm32f1) |
| `maudio_pads` | [maudio_pads](../maudio_pads/README.md) | the M-Audio Code pad LED board, RGB LEDs behind a serial link (stm32f1) |

Every item takes `id` and `platform`, the strip's `gamma` and `white`, and its platform's keys. Hubs are numbered in YAML order across all lists, so a strip's place in `led_strip:` is its place among the hubs.

## Pins

Pin n is colour n % c of LED n / c, with c colours an LED: 3 (0 red, 1 green, 2 blue), 4 on an RGBW strip (3 white). An LED whose `pin`, `pin2` and `pin3` are one LED's red, green and blue in a row shows any colour: full-colour looks (`idle: blue 20%`, `active: "#ff8000"`), cycle steps' colours and a DAW's colours ([Full-colour LEDs](../../core/README.md#full-colour-leds)). Any other pin is an on/off LED (0 or 255).

## Calibration

Two keys on the strip calibrate what goes on the wire, so colours look right:

- **`gamma:`** eyes see brightness on a curve: a quarter of the power looks about half as bright. Each colour goes through 255 × (v / 255) ^ gamma first, so `blue 50%` looks half as bright as `blue`. Default 1 (straight). About 2.2 suits most LEDs; anything above 0 stays above 0.
- **`white:`** what full red, green and blue (and white, on RGBW) send, to balance the LEDs: `[255, 200, 220]` takes some green and blue out where 255/255/255 looks green. Default all 255.

To find them: show a ramp (255, 64, 16, 4) and see whether 64 looks about half as bright (gamma near 2.2), then show a grid of whites and pick the most neutral. The Code 61's values (`gamma: 2.2`, `white: [255, 200, 220]`) came from that.

## Files

- `led_strip.h` / `led_strip.c`: `gm_led_strip_colour_out()`, a colour's byte on the wire (the curve, then the white).
- `__init__.py`: `LedStrip`, the `HubType` a platform's builder part derives from (`domain = "led_strip"`, outputs in the hub RAM, full-colour), the gamma table it writes (`c_tables`, `c_curve`), and `validate_rgb_calibration` / `rgb_curve`.

## Adding a strip platform

A component of its own, `components/<platform>/`, as [ws2812](../ws2812/) does:

1. **`<platform>.h`**: its config struct (with `curve` and `white`) and state, which the generated `gm_components.h` adds to the hub unions.
2. **`<platform>.c`**: its hub ops (`init` takes its bytes with `gm_hub_ram_alloc`, `write` and `rgb` set them, `poll` sends what changed through `gm_led_strip_colour_out`) and a `const gm_hub_ops_t gm_<platform>_ops`.
3. **`__init__.py`**: an `LedStrip` subclass (`key` = the platform name, `enum`, `num`, `keys` with `"platform"`, `lines`, `old_keys`, `validate` with `validate_rgb_calibration`, `range_text`, `colour_pins` if an LED isn't 3 pins, `c_fields` with `.curve = {self.c_curve(hub)}`, `c_header`, `c_config`, `c_state`, `c_ops`), listed in `HUB_TYPES`, with `DEPENDENCIES = ("led_strip",)`. Add its `GM_HUB_*` value to `core/include/gm_device.h`. Then `gemifi_build.py schema -o site/src/schema/device.json`.
4. **`test/`**: a host test with its YAML, built by `core/test/Makefile`.
