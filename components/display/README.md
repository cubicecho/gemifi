# display

The base component of **screens**: the panels a device's YAML lists under `display:`, ESPHome style, each on a platform. Text displays (`displays:`, `type: text`) name the screen they are on with `display: <id>`.

```yaml
i2c: { sda: GPIO21, scl: GPIO22 }
display:
  - { id: oled, platform: ssd1306, address: 0x3C }
displays:
  - { id: oled_main, display: oled, line: 1, scale: 2, shows: last_message }
  - { id: oled_channel, display: oled, line: 6, shows: channel }
```

`display:` and `displays:` are two things. A `display:` item is the hardware: a panel and its driver. A `displays:` item is a line of text the device shows: what it shows, a host's text, the device's messages ([core/README.md "Displays"](../../core/README.md#displays)). A screen can hold several text displays, a line each or side by side.

| Platform | Component | What |
|---|---|---|
| `ssd1306` | [ssd1306](../ssd1306/README.md) | SSD1306 and SH1106 I2C OLEDs (ESP only for now) |

Every item takes `id` (what text displays name) and `platform`; the rest are the platform's keys. Up to 8 screens.

## How it works

- **Framebuffer.** This base keeps each screen's framebuffer, width × height / 8 bytes (a byte is 8 pixels down, bit 0 at the top, at page × width + x), and a copy of what the panel was sent: `GM_SCREEN_RAM` bytes twice. Text displays draw 5 × 7 characters into it (`gm_screen_draw()`, from `core/src/display.c`), marking the pages that changed.
- **Sending.** From the main loop, after the hubs: the screens share what the hubs left of the bus budget (`vpin_poll` returns it; `gm_screen_send()`), never on the LED update's pass. At start-up and after the lamp test everything goes out at once.
- **A platform** implements `gm_screen_ops_t.send`: set the panel up the first time (`never`), then send the changed bytes of the marked pages within the budget.

## Files

- `gm_screen.h`: `gm_screen_t` (platform, width, height, the platform's config), `gm_screen_ops_t`, `gm_screens[]` (generated), `gm_screen_init()`, `gm_screen_draw()`, `gm_screen_send()`.
- `screen.c`: the framebuffers, the drawing and the send loop.
- `__init__.py`: `ScreenType`, the class a platform's builder part derives from.

## Adding a display platform

A component of its own, `components/<platform>/`, as [ssd1306](../ssd1306/) does:

1. **`<platform>.h`**: its config struct (a member of `gm_screen_t`'s union) and flags.
2. **`<platform>.c`**: its `send` and a `const gm_screen_ops_t gm_<platform>_ops`.
3. **`__init__.py`**: a `ScreenType` subclass (`platform`, `enum`, `num`, `keys`, `i2c`, `schema`, `enums`, `c_header`, `c_config`, `c_ops`, `validate`, `c_fields`), listed in `DISPLAY_PLATFORMS`, with `DEPENDENCIES = ("display",)`. Add its `GM_SCREEN_*` value to `gm_screen.h`. Then `gemifi_build.py schema -o site/src/schema/device.json`.
4. **`test/`**: a host test with its YAML, built by `core/test/Makefile`.

Not yet: screens that take text rather than pixels (HD44780 character LCDs: they'd add a text op to `gm_screen_ops_t`), SPI panels, graphics.
