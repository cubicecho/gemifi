# ws2812

A hub type ([hub](../hub/gm_hub.h), [core/README.md "Virtual pins and hubs"](../../core/README.md#virtual-pins-and-hubs)): `ws2812.c` (the driver), `ws2812.h` (its config and state), `__init__.py` (the YAML: validation and generated C), `test/` (a host test, run by `core/test`).

`ws2812` drives a chain of addressable LEDs, WS2812B or SK6812 (RGB or RGBW), on one MCU pin. Each LED is 3 hub pins (4 on RGBW) as on `maudio_pad_leds`, so LEDs take `pin`, `pin2`, `pin3` and show any colour ([Full-colour LEDs](../../core/README.md#full-colour-leds)):

```yaml
ws2812:
  - { id: strip, pin: GPIO48, count: 16, order: GRB, gamma: 2.2, white: [255, 235, 210] }
led_defaults: { idle: blue 20%, active: white }       # full colours where an LED can show them
leds:
  - { id: led_1, pin: { ws2812: strip, number: 0 }, pin2: { ws2812: strip, number: 1 }, pin3: { ws2812: strip, number: 2 } }
  - { id: led_2, pin: { ws2812: strip, number: 3 }, pin2: { ws2812: strip, number: 4 }, pin3: { ws2812: strip, number: 5 }, idle: "#ff6000" }
```

- **`order:`** the colours in the order the LED takes them: `GRB` (WS2812B, SK6812 RGB; the default), `RGB`, `BRG`, ..., and `GRBW`, `RGBW`, ... for RGBW LEDs (SK6812 RGBW). With a W, each LED is 4 pins (red, green, blue, white) and `white:` takes 4 values. The white part of a colour (the least of red, green and blue) goes on the white LED, so `white` or `#ffffff` lights only the white LED; the white pin on its own (`{ ws2812: strip, number: 4 * LED + 3 }`) is a single-colour LED.
- **When a frame goes out:** pins only change a copy in RAM; the main loop builds a frame (order, `gamma:`, `white:` applied) only when a colour changed, and sends it only if it differs from the last, at most every 5 ms (`WS_FRAME_MS`), never while the last frame and its latch (280 us low) are still going out, and never from the scan interrupt. The platform sends it in the background (`hal_ledstrip_*`), so the main loop doesn't wait either: a frame is 30 us an LED on the wire (16 LEDs about 0.5 ms).
- **ESP32, S2, S3:** RMT, ESP-IDF's TX driver with a bytes encoder (`platform/esp/gemifi_esp/ledstrip_esp.c`): any output pin, a TX channel a strip (ESP32 8, S2 and S3 4).
- **stm32f1:** the SPI's MOSI with DMA: PA7 (SPI1, DMA1 channel 3) or PB15 (SPI2, channel 5), one strip each. The SPI runs at about 2.25 MHz and each LED bit is three SPI bits (0 = 100, 1 = 110: 0.44 / 0.89 us high, 1.33 us a bit). The DMA channel goes through the allocator ([DMA channels](../../core/README.md#dma-channels-stm32f1)). RAM: the hub's bytes twice (the colours and the last frame), and 3 bytes per colour byte for the SPI frame, plus 91.
- **Not supported:** the NUC (no driver yet); the builder refuses it. APA102 (clock and data) isn't there yet.
- **Wiring:** WS2812B take 5 V data; at 3.3 V most work, a 74AHCT125 level shifter makes sure. A resistor (about 330 ohm) in the data line and a big capacitor across the strip's supply are good practice; at full white a WS2812B draws about 60 mA.
