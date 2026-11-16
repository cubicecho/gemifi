# i2c

The base component of the I2C buses, ESPHome style: the SDA and SCL pins of each bus the device's I2C chips are on.

```yaml
i2c: { sda: GPIO21, scl: GPIO22 }
mcp23017:
  - { id: io, address: 0x20 }
```

That is one bus. A device can have several, `i2c:` a list of them, each with an `id`; a component on I2C names its bus with `i2c_id:`:

```yaml
i2c:
  - { id: bus_a, sda: GPIO21, scl: GPIO22 }
  - { id: bus_b, sda: GPIO25, scl: GPIO27 }
mcp23017:
  - { id: io, address: 0x20, i2c_id: bus_a }
display:
  - { id: oled, platform: ssd1306, i2c_id: bus_b }
```

<!-- keys -->

- **On I2C:** the `mcp23017` and `pcf8574` hubs, the `ht16k33` LED driver platform and the `ssd1306` display platform. Each takes `i2c_id:`, which may be left out when the device has one bus; with two or more the builder asks for it.
- **Addresses** must differ on a bus; the same address on two buses is fine (two OLEDs at 0x3C).
- **How many:** the ESP32, S2 and S3 have two I2C controllers, so two buses. The stm32f1 and nuvoton-nuc1xx have no I2C driver yet: the builder refuses I2C components there.
- **The platform** drives them: `hal_i2c_write(bus, addr, ...)` / `hal_i2c_read(bus, addr, ...)` (`core/include/gm_hal.h`), `bus` the bus's index in the YAML, its pins from `GM_I2C_PINS` in the generated `gm_device_config.h` (`GM_I2C_BUSES` of them). On ESP: one ESP-IDF master bus per entry, on a free controller, with the internal pull-ups on, at 400 kHz (`platform/esp/gemifi_esp/hal_esp.c`). I2C runs from the main loop only, never from the scan interrupt.

Not yet: `frequency:`, `scan:`, I2C on the stm32f1 and NUC1xx.
