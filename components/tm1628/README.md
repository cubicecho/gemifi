# tm1628

A hub type ([hub](../hub/gm_hub.h), [core/README.md "Virtual pins and hubs"](../../core/README.md#virtual-pins-and-hubs)): `tm1628.c` (the driver), `__init__.py` (the YAML: validation and generated C). Its config (`gm_hub_tm1628_t`, `include/gm_device.h`) and state (`gm_hub.h` `u.tm`) stay in the core, with `fade:` (`gm_vpin.h` `vpin_fade*`, `core/src/vpin.c`), which sets the chip's brightness from them. Its host test is `core/test/test_tm1628.c`, with the M-Audio pad board's.

`tm1628` covers the TM1628 and its relatives that take the same commands (TM1638, TM1620, …: set `ram_bytes` to what the chip has or what the board uses). They're output-only here: the key-scan inputs aren't read.

- **Pin numbers** are display RAM bits: `number = byte * 8 + bit`, bit 0 = the LSB of the byte. `{ tm1628: leds_a, number: 37 }` is byte 4 (address 0xC4), bit 5. On a TM1628 in its usual wiring, byte 2g is SEG1–SEG8 of grid g+1 and byte 2g+1 is SEG9–SEG14, but the board decides what each bit lights.
- **Sharing**: several chips can share `clk_pin` and `dio_pin` (`allow_other_uses: true` on each), each with its own `stb_pin`. TM1628 lines can't be shared with other hubs' lines.
- **Writing**: `vpin_write` only changes a RAM copy (the hub RAM, `GM_HUB_RAM` bytes in `core/src/vpin.c`) and marks the chip dirty if a bit changed. The main loop writes dirty chips, never the scan interrupt: at most 4 bytes on the bus per pass, and not on the pass that works the LED levels out (every 10 ms). A chip's first write, at start-up, is its whole RAM: the display-mode command (`grids - 4`, only if `grids:` is set), 0x40 (write, auto-increment), 0xC0 and all `ram_bytes` bytes, then 0x88 | `intensity` (display on). Those stay set in the chip, so after that only what changed goes out: 0x40, then each run of changed bytes as one command, 0xC0 | its first address and its bytes. A run goes on over one unchanged byte (sending it again costs a bus byte, as a new address command would) and ends at two. One LED is 3 bus bytes (0x40, C0|a, the byte), as a fixed-address write (0x44) would be; 12 changed bytes in a row (about what the Code 61's 6-character display takes) are 14, where fixed addresses would take 25. A run the 4-byte budget cuts short goes on at its next address (a new 0xC0 | address) on the next pass. Bits go LSB first, DIO set while CLK is low and taken on the rising edge; the bus idles with STB, CLK and DIO high. Each chip is written once at start-up (all off after the lamp test), so it's cleared even with no LEDs on it.
- **Timing**: CLK low 600 ns (DIO is set at its start, so that's also DIO's setup time: an open-drain DIO has to rise through its pull-up within it), CLK high 450 ns, so a bit is at least 1.05 µs (the TM1628 takes up to 1 MHz, PWCLK ≥ 400 ns, setup and hold ≥ 100 ns); STB low 1 µs before the first bit, high 1 µs after the last one, and high for at least 1 µs between commands. The phases are counted on `hal_cycles()` from just after each edge (`hal_ns_to_cycles` converts, once per write), and the pins are written with one set/reset store each (`hal_pin_bsrr`), so on the STM32 at 72 MHz a bit is about 1.3 µs and a byte about 11 µs: the 4-byte budget is ~50 µs a pass, one LED ~40 µs. A platform without them (`hal_pin_bsrr` or `hal_ns_to_cycles` returning 0) uses `hal_pin_write` and `hal_delay_us(1)` a phase, about 2 µs a bit or more. `TM_CLK_LOW_NS` and `TM_CLK_HIGH_NS` in `tm1628.c` set the phases; raise the low one if a slow DIO pull-up garbles bits.
- **Brightness** is per chip (`intensity`, the chip's PWM). LEDs on it are on/off, like other LEDs.
- **`fade: true`** lets a chip's brightness follow the host: a DAW's colour (FB_LED_RGB) sets the chip's 8 steps from the brightest LED on it, up to `intensity`, and below half the dimmest step the chip goes dark (display off, 0x80). Host colours win: any other lit LED or look counts as full only while no LED on the chip shows a host colour, so an LED the host turned off (showing its idle look) doesn't hold the chip at full. Every LED on the chip dims together, so it suits a chip with only LEDs on it (the Code 61's button chip) and not one that also drives a display. The steps are uneven (pulse widths of 1, 2, 4, 10, 11, 12, 13 and 14 sixteenths), so a fade is coarse at the bottom and nearly flat at the top.

```yaml
tm1628:
  - id: leds_a
    clk_pin: { number: PC10, allow_other_uses: true }
    dio_pin: { number: PC11, mode: OUTPUT_OPEN_DRAIN, allow_other_uses: true }
    stb_pin: PA15
  - id: leds_b
    clk_pin: { number: PC10, allow_other_uses: true }
    dio_pin: { number: PC11, mode: OUTPUT_OPEN_DRAIN, allow_other_uses: true }
    stb_pin: PC12
    ram_bytes: 6
leds:
  - { id: shift_led, pin: { tm1628: leds_a, number: 37 } }
  - { id: pad_1, pin: { tm1628: leds_b, number: 0 }, pin2: { tm1628: leds_b, number: 1 }, pin3: { tm1628: leds_b, number: 2 } }
```

**Finding which bit lights what** (a board whose map is unknown): make a test YAML with one LED per bit, with no buttons using them, so the host drives them all:

```yaml
leds:
  - { id: a_0, pin: { tm1628: leds_a, number: 0 }, feedback: { note: 0 } }
  - { id: a_1, pin: { tm1628: leds_a, number: 1 }, feedback: { note: 1 } }
  # ... one per bit; the second chip on channel 2: feedback: { note: 0, channel: 2 }
```

Build and upload it, then light one bit at a time: from the web app's host-feedback card (each LED is listed by name, with Off / Blink / On), or by sending Note On to the LED's note from any MIDI tool. Note which LED or segment lights for each bit; then replace the list with the real LEDs. The lamp test at start-up lights every listed bit for 0.2 s.
