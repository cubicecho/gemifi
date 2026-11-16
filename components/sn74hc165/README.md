# sn74hc165

A hub type ([hub](../hub/README.md)): a chain of 74HC165 parallel-in shift registers, 8 inputs a chip on three MCU pins, for buttons and key-matrix columns.

```yaml
sn74hc165:
  - { id: switches, data_pin: PB12, clock_pin: PB13, load_pin: PB14, sr_count: 2 }
buttons:
  - { id: fs_1, pin: { sn74hc165: switches, number: 0, inverted: true }, action: cc_momentary, cc: 21 }
```

<!-- keys -->

Hub types and the pins they give: [hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components).

The inputs are numbered as ESPHome does: 0–7 on the chip whose QH goes to the MCU (0 = its A), 8–15 on the next. They are read once per scan into a cache, from the scan tick, so a whole key matrix can live on shift registers.

## Files

- `sn74hc165.c`: the driver.
- `__init__.py`: the YAML: a `ShiftRegister` of the hub base. Its config (`.sr`, `include/gm_device.h`) is shared with the `sn74hc595`, and its pins are the cached `in` word (`GM_HUBF_IN_REG`: a key scan reads a column with one load).
