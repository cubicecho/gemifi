# leds

A control ([core/README.md "Controls"](../../core/README.md#controls)): single-colour, bicolour and RGB LEDs on any output pin (an MCU pin, a shift register, an LED driver's, an LED strip's). Its YAML (`leds:`, `led_defaults:`): the fields in [core/README.md "Controls"](../../core/README.md#controls), "LED looks", "Full-colour LEDs" and "LED walk" below, and who drives an LED in [core/README.md "LED ownership"](../../core/README.md#led-ownership).

- `leds.c`: what each LED shows, its looks, who owns it, the TM1628 fade, the LED walk and the encoder rings. Always built (`REQUIRED`, not `in_device`, so not in `components.controls()`): the status LED and the lamp test use it on any device.
- `__init__.py`: the builder part: the looks and the owners, `validate` (its YAML: pins, looks, `led_defaults:`, owner, timeout, feedback source) and `c_tables` (`gm_led_t leds[]`), and that it's always built (`REQUIRED`). Host feedback's per-LED table (`gm_led_fb[]`) stays with the builder's feedback code.

## LED looks (idle and active)

By default an LED is dark while off and lit (white on an RGB LED) while on. `idle:` and `active:` change that: `idle:` is what it shows while off, so a panel can be lit all the time and still show presses, and `active:` what it shows for "on" (held, toggled on, a pad hit, a host's on).

```yaml
led_defaults: { idle: blue, active: white }   # optional: every LED, unless it sets its own (full colours too: blue 20%)
leds:
  - { id: rec_led, pin: PC4, idle: on }                                # lit until pressed (active: off)
  - { id: bi_led, pin: PC5, pin2: PC6, idle: colour2, active: colour1 }
  - { id: b1_led, pin: PB0, pin2: PB1, pin3: PB2, idle: blue, active: white }
```

| LED | Looks | Defaults (idle / active) |
|---|---|---|
| single colour (`pin`) | `off`, `on` | off / on |
| bicolour (`pin2`) | `off`, `colour1` (`pin`), `colour2` (`pin2`), `both`; `on` = colour1 | off / colour1 |
| RGB (`pin3`) | `off`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `white`; `on` = white | off / white |
| RGB on an LED strip (`led_strip:`) | any colour too: `"#rrggbb"` (quoted: `#` starts a YAML comment), or a colour name with a brightness, `blue 20%` | off / white |

- `on` and `off` work with or without quotes.
- **`active:` left out** with an `idle:` that matches the default active look: active is `off`. So `idle: on` on a single-colour LED means lit until pressed, then dark. Giving both the same look is a warning: the LED would never show its state.
- **`led_defaults:`** takes any look of any kind of LED, full colours included, and each LED shows the nearest it can: a full colour as it is on an LED that can show any colour, elsewhere the channels at least half the brightest (`"#ff8000"` is red + green, yellow; black is `off`); a named look the same pins where it has them (yellow = red + green = `both` on a bicolour LED), else its first colour. A single-colour LED is `on` for anything but `off`, and then dark when active if the two come out the same. An LED's own `idle:` / `active:` win over the defaults.
- **What follows the looks:** off (0) shows idle and a plain on shows active, from every source: buttons, pads, modes, plain host feedback and Mackie Control. A blink alternates active and idle. A bicolour LED's other levels keep their meaning (octave +2 both colours, +3 colour 2, +4 colour 2 blinking against idle), and so does a colour a DAW script picks (FB_LED_RGB). The lamp test at start and the LED walk light every pin whatever the looks.
- The TM1628 and the MCU pins are on or off, so idle can't be a dim version of active there: pick a different colour, or `on` / `off`. Only an LED of an LED strip (`led_strip:`; "Full-colour LEDs") takes full colours in its own `idle:` / `active:` (`idle: blue 20%`, `active: "#ff8000"`); elsewhere they are an error. Brightness in a full colour is as seen: the hub's `gamma:` turns it into power.
- **Generated:** `gm_led_t.looks` = `GM_LED_LOOKS(idle, active)`, each a pin mask (bit 0 `pin`, 1 `pin2`, 2 `pin3`), or 0 for the defaults; `gm_led_t.rgb` = the full colours, `GM_LED_RGB(0xRRGGBB)` each, or 0. A full-colour look's mask is its nearest on/off colour.

## Full-colour LEDs

LED strips (`led_strip:`, [components/led_strip](../led_strip/README.md)) drive LEDs that can show any colour, a byte per colour: `ws2812` (addressable strips) and `maudio_pads` (the M-Audio Code pad LED board). An RGB LED on one of them (`pin`, `pin2`, `pin3` = one LED's red, green and blue: numbers LED × 3, + 1, + 2; LED × 4 on an RGBW strip) shows:

- full-colour looks: `idle: blue 20%`, `active: "#ff8000"` ("LED looks"), from its own `idle:` / `active:` or `led_defaults:`;
- a cycle step's full colour (`colour: "#ff8000"`, [modes "Cycles"](../modes/README.md#cycles));
- the colour a DAW script picks (FB_LED_RGB, 0..127 per channel, sent as 0..255).

Anything that only switches pins on or off (a plain on, a named look like `blue`, a mode's named colour) sends 0 or 255 per colour. An LED whose pins aren't one LED's red, green and blue in a row (or an LED elsewhere) is on/off per pin: a full-colour look there is an error, and in `led_defaults:` or a step colour it shows as the nearest on/off look.

The strip's `gamma:` and `white:` calibrate what goes on the wire, so colours look right ([led_strip "Calibration"](../led_strip/README.md#calibration)).

## LED walk (finding LEDs)

To find which output drives which LED on new hardware, list every candidate output under `leds:` and build with `GM_LED_WALK_MS` set (stm32f1: `make LED_WALK=100`). Each LED then lights on its own for that many milliseconds, in `leds:` order, and the device log shows `walk LED <n>`. Holding any button pauses the walk. Normal LED behaviour is off in such a build; without the define the code isn't built at all.

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

It reads the buttons' `btn_held`, `btn_toggled` and `ramp_q8`, and the pads' `pad_state`, `pad_lit` and `pad_owner` (which of a pad and its twins took the hit, so lights its LED).

## Tests

In `core/test/`: `test_leds.c` (`leds-test.yaml`, `leds-local-test.yaml` and a 132-LED variant on 74HC595 chains: the looks, the lamp test, steady LEDs not rewritten, host and DAW-script levels in the looks, the octave levels), `test_feedback.c` (host levels, owners and timeouts, rings) and `test_tm1628.c` (the fade).
