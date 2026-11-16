# cd74hc4067

Two hub types with one driver ([hub](../hub/README.md)), `cd74hc4051` and `cd74hc4067`: CD74HC4051 / 4067 multiplexers, which route one common pin to 8 or 16 input channels, so a device reads 8 or 16 knobs, faders, pads or buttons on one MCU pin.

```yaml
cd74hc4067:
  - { id: knobs, pin_s0: GPIO39, pin_s1: GPIO40, pin_s2: GPIO41, pin_s3: GPIO42, common_pin: GPIO2 }
cd74hc4051:
  - { id: faders, pin_s0: GPIO4, pin_s1: GPIO5, pin_s2: GPIO6, common_pin: GPIO1, delay: 5us }
analog:
  - { id: knob_1, pin: { cd74hc4067: knobs, number: 0 }, cc: 70 }
  - { id: fader_1, pin: { cd74hc4051: faders, number: 0 }, cc: 20 }
```

<!-- keys -->

Hub types and the pins they give: [hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components). A `cd74hc4051` has 8 input channels, a `cd74hc4067` 16, analog (if `common_pin` is an ADC pin) or digital, and a pin names the mux by its key: `{ cd74hc4051: faders, number: 0 }`.

## Shared select lines

Muxes may share select lines (`allow_other_uses: true` on each use); give them the same `delay`. Switching one switches the others: one with the same lines is then on the same channel, one that shares only some switches again before its next read.

## Files

- `cd74hc4067.c`: the driver of both types (both `GM_HUB_MUX`): control pins and the analog op.
- `__init__.py`: the YAML: both keys.
- In the core: its config (`.mux`, `include/gm_device.h`), with the scan's path in `core/src/vpin.c`: `vpin_read` reads a channel digitally itself, `gm_hub_mux_select` switches channels (and the muxes that share select lines), and the hardware analog scan (`gm_adc_plan`) steps the select lines.
- Its host test is `core/test/test_pads.c` (pads on shared-select muxes, read in software and by the hardware scan).
