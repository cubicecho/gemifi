# cd74hc4067

Two hub types with one driver ([hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components)), `cd74hc4051` and `cd74hc4067` (both `GM_HUB_MUX`): `cd74hc4067.c` (the driver: control pins and the analog op), `__init__.py` (the YAML: both keys). Its config (`.mux`, `include/gm_device.h`) stays in the core, with the scan's path in `core/src/vpin.c`: `vpin_read` reads a channel digitally itself, `gm_hub_mux_select` switches channels (and the muxes that share select lines), and the hardware analog scan (`gm_adc_plan`) steps the select lines. Its host test is `core/test/test_pads.c` (pads on shared-select muxes, read in software and by the hardware scan).

- **`cd74hc4051`**: `pin_s0`–`pin_s2`, `common_pin`, optional `enable_pin` (/E, driven low), `delay` (settle time after switching channel, default 2us). 8 input channels, analog (if `common_pin` is an ADC pin) or digital.
- **`cd74hc4067`**: `pin_s0`–`pin_s3`, `common_pin`, optional `enable_pin`, `delay`. 16 input channels, as above.

A pin names the mux by its key: `{ cd74hc4051: faders, number: 0 }`. Muxes may share select lines (`allow_other_uses: true` on each use); give them the same `delay`. Switching one switches the others: one with the same lines is then on the same channel, one that shares only some switches again before its next read.
