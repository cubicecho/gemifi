# xy_pad

A hub type ([hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components)): `xy_pad.c` (the driver), `xy_pad.h` (its config), `__init__.py` (the YAML: validation and generated C). Its state (`gm_hub.h` `u.xy`) stays in the core, with `vpin_analog_jump` (`gm_vpin.h`), which reads it; the hardware analog scan leaves the pad out of its plan (`gm_adc_plan`). Its host test is `core/test/test_pads.c`, with the pads'.

A 4-wire resistive XY pad is two plates that touch where pressed. To read X, the core drives `x_plus_pin` high and `x_minus_pin` low, and reads the voltage of the floating Y plate. That reading is the touch position. Y is read the other way round. Each axis is read on the other plate's minus pin if that pin has an ADC, otherwise on its plus pin. The other plate is always released before a plate is driven, so a touch never joins two driven pins.

The core reads one axis per millisecond, in turn, in software. Each plate gets a full millisecond to settle, and each axis is new every 2 ms. On stm32f1 the reads use ADC2, so the pad works alongside `hardware_scan.analog`. That scan leaves the pad out of its plan.

A touch counts once both readings have been inside `touch_min`..`touch_max` four times in a row (each axis twice, about 4 ms). The first reading as a finger lands can be anywhere. Untouched, the floating plate reads outside that window. While untouched, the inputs send nothing and hold their last value. A new touch jumps straight to its position, without gliding there through the smoothing filter.

```yaml
xy_pad:
  - { id: xy, x_plus_pin: PB1, x_minus_pin: PC5, y_plus_pin: PC4, y_minus_pin: PB2 }
analog:
  - { id: xy_x, pin: { xy_pad: xy, number: x }, cc: 16, adc_min: 592, adc_max: 3408 }
  - { id: xy_y, pin: { xy_pad: xy, number: y }, cc: 17, adc_min: 592, adc_max: 3408 }
```

The axes are ordinary analog inputs: CC or channel pressure, per-preset settings, calibration with `adc_min`/`adc_max`. They can't be `pitch_bend`, because bend centres on the power-up reading and the pad reads nothing until it's touched.
