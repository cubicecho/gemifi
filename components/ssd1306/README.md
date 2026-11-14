# ssd1306

A display platform ([display](../display/README.md)): the small I2C OLEDs, 0.96" 128 × 64 and 0.91" 128 × 32 SSD1306 modules and 1.3" SH1106 ones, written from the main loop on the I2C bus (ESP only for now). `ssd1306.c` (the driver), `ssd1306.h` (its config), `__init__.py` (the YAML), `test/` (a host test on a fake bus, run by `core/test`).

```yaml
i2c: { sda: GPIO21, scl: GPIO22 }
display:
  - { id: oled, platform: ssd1306, address: 0x3C }          # model: sh1106 for a 1.3" module
displays:
  - { id: oled_main, display: oled, line: 1, scale: 2, shows: last_message }   # 10 big characters
  - { id: oled_channel, display: oled, line: 6, shows: channel }              # 21 small ones
```

| Key | |
|---|---|
| `address` | the 7-bit I2C address, default 0x3C. 0x78 / 0x7A, the 8-bit forms printed on many boards, are taken as 0x3C / 0x3D with a warning |
| `model` | `ssd1306` (the default) or `sh1106` |
| `width` × `height` | 128 × 64 (the default); an SSD1306 also 128 × 32, 96 × 16, 64 × 48, 64 × 32 |
| `contrast` | 0–255, default 207 |
| `rotate` | 0 or 180 |

The display base keeps its framebuffer (1 KB for 128 × 64, and as much again for what the panel was sent).

- **The first write** sets the panel up with the display off (clock, multiplex = height − 1, offset 0, start line 0; on an SSD1306 the charge pump on and page addressing, on an SH1106 its DC-DC on; segment remap and COM scan direction, reversed with `rotate: 180`; COM pins, `contrast`, pre-charge, VCOMH), writes every page (blank), then turns the display on, so nothing random shows.
- **After that**, each pass sends one run of changed bytes: its page and column (three commands, each after a 0x80 control byte), then 0x40 and up to 16 data bytes, one write of at most 23 bytes (~0.55 ms at 400 kHz; the ESP Wire buffer is 128). A run is bigger than the shared 4-byte budget, so a pass sends one and nothing else on the bus, and not on the LED update's pass. A whole new line of 10 characters at scale 2 (240 bytes) takes 15 passes. Only changed bytes go out, so a CC value changing on a big display rewrites a few characters' columns.
- **The SH1106** has 132 columns of RAM and shows 128 from column 2, and it only has page addressing: give `model: sh1106`. If the text is upside down, `rotate: 180`. If nothing shows, check the address: the module's 0x78 (as printed) is 0x3C, its 0x7A is 0x3D.
- The writes aren't checked, as the I2C hubs' aren't: a missing panel costs its bytes once per change.

An OLED used to be a hub, `ssd1306: [{ id: oled, ... }]`, and a text display named it with `ssd1306: oled`. The builder refuses both now, with the new form in the message.
