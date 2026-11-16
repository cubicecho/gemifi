# mcp23017

A hub type ([hub](../hub/README.md)): an MCP23017, 16 GPIO on the I2C bus, so a device gets 16 more input or output pins on the two I2C pins.

```yaml
i2c: { sda: GPIO8, scl: GPIO9 }
mcp23017:
  - { id: io, address: 0x20 }
leds:
  - { id: play_led, pin: { mcp23xxx: io, number: GPB0 } }
```

<!-- keys -->

Its bus is an [`i2c:`](../i2c/README.md) one. Hub types and the pins they give: [hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components).

Its 16 GPIO are numbered `0`–`15` or `GPA0`–`GPB7`, with pull-ups (`mode: INPUT_PULLUP`). Its pin key is `mcp23xxx`, as in ESPHome: `{ mcp23xxx: io, number: GPB7 }`. It is read and written from the main loop (every 1 ms), since I2C can't run in the scan interrupt. A chip with no input on it is never read (a read is two transactions, ~120 µs at 400 kHz), and outputs are written only when they change; the writes aren't checked. ESP only for now: stm32f1 and nuvoton-nuc1xx have no I2C driver, so the builder refuses it there.

## Files

- `mcp23017.c`: the driver.
- `__init__.py`: the YAML: an `I2CExpander` of the hub base. Its config (`.i2c`, the address) and pin state (`u.io`, `gm_hub_i2c_*`) are the hub base's, shared with the `pcf8574`.
- Its host test is `core/test/test_i2c.c`, with the other I2C hubs'.
