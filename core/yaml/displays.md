# Displays

The text displays: a row of segment characters on a hub's output pins (a TM1628, MAX7219, TM1637, 74HC595 ...), or a row of text on a screen (a `display:` item, an OLED).

```yaml
displays:
  - id: display                # 14-segment characters on a hub
    chars:
      - { led_driver: leds_a, number: 48 }
      - { led_driver: leds_a, number: 64 }
  - id: preset                 # text on an OLED
    display: oled
    line: 2
    scale: 2
    shows: preset_name
```

<!-- keys -->

- **Up to 4 displays**, of up to 16 characters each (a text display up to 21), one row each. A segment display's keys and a text display's don't mix.
- **Wiring.** A character's segments are the pins after its first one, `segment_step` pins apart, or in `segment_order`; or a list of its segments' pins for any other wiring. Every segment pin counts as used ([core/README.md "Displays"](../README.md#displays), "Pins").
- **What it shows** of its own (`shows:`), the device's messages and a host's text (FB_TEXT): [core/README.md "Displays"](../README.md#displays).
- **The screens** a text display is on are `display:` items (singular), a component: [components/display](../../components/display/README.md).
