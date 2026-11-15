# leds

A control ([core/README.md "Controls"](../../core/README.md#controls)): single-colour, bicolour and RGB LEDs on any output pin (an MCU pin, a shift register, an LED driver's, an LED strip's). Its YAML (`leds:`, `led_defaults:`) and what each setting does: [core/README.md "Controls"](../../core/README.md#controls), "LED looks", "Full-colour LEDs", "LED walk" and "DAW feedback" ("LED ownership").

- `leds.c`: what each LED shows, its looks, who owns it, the TM1628 fade, the LED walk and the encoder rings. Always built (`REQUIRED`): the status LED and the lamp test use it on any device.
- `__init__.py`: the builder part: the looks and the owners, `validate` (its YAML: pins, looks, `led_defaults:`, owner, timeout, feedback source) and `c_tables` (`gm_led_t leds[]`), and that it's always built (`REQUIRED`). Host feedback's per-LED table (`gm_led_fb[]`) stays with the builder's feedback code.

## How it runs

The main loop (`gm_poll`) calls `leds_update` every `LED_MS` (10 ms). It works out each LED's level from:
- its button's state (held, toggled, a ramp, the octave or transpose);
- its pad's hit, which stays lit for at least `PAD_LED_MIN_MS`;
- a mode (`mode_led`);
- the status LED's blink code (`gm_safety.h`).

With host feedback (`GM_HOST_LEDS`), `leds_update` only works out the device's own levels, and `leds_apply` shows `LED_STEP` (13) LEDs per pass on the passes in between. That way no single pass carries every LED. For each LED, the owner (`owner:`, `timeout:`) decides whether the device's level or the host's wins.

An LED whose level and blink phase haven't changed isn't written again (`led_shown`). With a TM1628 fade, once a round is shown, `leds_fade` sets each chip's brightness from the LEDs' colours. The chips' bytes wait for that (`leds_round_open`).

`gm_init` calls `leds_init`, and `leds_lamp` for the lamp test (every LED on, then off). Host feedback calls `gm_led_host` and `gm_led_script` / `gm_led_script_rgb` (`gm_feedback.h`), and `gm_ring_draw` draws an encoder's ring of LEDs from its value.

## What it shares

Through `core/include/gm_controls.h`:
- the LED levels (`LV_*`), which `mode_led` returns;
- `led_shown` and `led_dead`, which the modes' `layers_update` writes when an LED changes hands between a base and its layer twin;
- the functions above, for `gm_poll` and `gm_init`.

It reads the buttons' `btn_held`, `btn_toggled` and `ramp_q8`, and the pads' `pad_state` and `pad_lit`.

## Tests

In `core/test/`: `test_leds.c` (`leds-test.yaml`, `leds-local-test.yaml` and a many-LED variant: looks, owners, timeouts, apply rounds), `test_feedback.c` (host levels, rings) and `test_tm1628.c` (the fade).
