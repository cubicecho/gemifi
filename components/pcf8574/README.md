# pcf8574

A hub type ([hub](../hub/README.md)): a PCF8574 (or PCF8574A), 8 quasi-bidirectional GPIO on the I2C bus, so a device gets 8 more input or output pins on the two I2C pins.

```yaml
i2c: { sda: GPIO8, scl: GPIO9 }
pcf8574:
  - { id: pcf, address: 0x38 }
buttons:
  - { name: PCF 0, pin: { pcf8574: pcf, number: P0, inverted: true }, action: program_next }
```

Its keys are below; its bus is an [`i2c:`](../i2c/README.md) one. Hub types and the pins they give: [hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components).

`pcf8574` is a PCF8574 (or PCF8574A): 8 GPIO on the I2C bus (`address`; 0x20-0x27, a PCF8574A 0x38-0x3F; an [`i2c:`](../i2c/README.md) bus, `i2c_id:`), numbered `0`–`7` or `P0`–`P7`. They are quasi-bidirectional: inputs are written high. It is read and written from the main loop (every 1 ms), since I2C can't run in the scan interrupt. A chip with no input on it is never read, and outputs are written only when they change; the writes aren't checked. ESP only for now, as the `mcp23017`.

## Files

- `pcf8574.c`: the driver.
- `__init__.py`: the YAML: an `I2CExpander` of the hub base. Its config (`.i2c`, the address) and pin state (`u.io`, `gm_hub_i2c_*`) are the hub base's, shared with the `mcp23017`.
- Its host test is `core/test/test_i2c.c`, with the other I2C hubs'.
