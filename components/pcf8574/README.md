# pcf8574

A hub type ([hub](../hub/gm_hub.h), [core/README.md "Virtual pins and hubs"](../../core/README.md#virtual-pins-and-hubs)): `pcf8574.c` (the driver), `__init__.py` (the YAML: an `I2CExpander` of the hub base). Its config (`.i2c`, the address) and pin state (`u.io`, `gm_hub_i2c_*`) are the hub base's, shared with the `mcp23017`. Its host test is `core/test/test_i2c.c`, with the other I2C hubs'.

`pcf8574` is a PCF8574 (or PCF8574A): 8 GPIO on the I2C bus (`address`; 0x20-0x27, a PCF8574A 0x38-0x3F; the `i2c:` pins), numbered `0`–`7` or `P0`–`P7`. They are quasi-bidirectional: inputs are written high. It is read and written from the main loop (every 1 ms), since I2C can't run in the scan interrupt. A chip with no input on it is never read, and outputs are written only when they change; the writes aren't checked. ESP only for now, as the `mcp23017`.
