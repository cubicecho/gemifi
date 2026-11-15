# sn74hc165

A hub type ([hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components)): `sn74hc165.c` (the driver), `__init__.py` (the YAML: a `ShiftRegister` of the hub base). Its config (`.sr`, `include/gm_device.h`) is shared with the `sn74hc595`, and its pins are the cached `in` word (`GM_HUBF_IN_REG`: a key scan reads a column with one load).

`sn74hc165` is a chain of 74HC165s: `data_pin` (QH), `clock_pin` (CLK), `load_pin` (SH/LD, loads when low); `sr_count` chips (1–4), 8 inputs each, numbered as ESPHome does: 0–7 on the chip whose QH goes to the MCU (0 = its A), 8–15 on the next. Buttons and key-matrix columns. The inputs are read once per scan into a cache, from the scan tick, so a whole key matrix can live on shift registers.
