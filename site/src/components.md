---
layout: base.njk
title: Components
description: What a gemifi device YAML can describe (keys, buttons, encoders, pads, faders and wheels, LEDs, displays, expander chips, DIN MIDI, presets and DAW feedback) and what each one costs.
permalink: /components/
---
<section class="hero">
  <h1>Components</h1>
  <p>What a device YAML can describe, what each part does, and what it costs.</p>
</section>

<section class="section">
  <h2>On this page</h2>
  <ul class="toc">
    <li><a href="#basics">The basics</a></li>
    <li><a href="#keys">Keys</a></li>
    <li><a href="#buttons">Buttons, modes and layers</a></li>
    <li><a href="#encoders">Encoders</a></li>
    <li><a href="#analog">Faders, knobs and wheels</a></li>
    <li><a href="#messages">MIDI messages</a></li>
    <li><a href="#pads">Pads</a></li>
    <li><a href="#leds">LEDs</a></li>
    <li><a href="#displays">Displays</a></li>
    <li><a href="#hubs">Expander chips</a></li>
    <li><a href="#din">DIN MIDI</a></li>
    <li><a href="#presets">Settings and presets</a></li>
    <li><a href="#feedback">DAW feedback</a></li>
  </ul>
</section>

<div class="prose">

A device YAML lists the hardware, one section per kind of part. Every part
has an `id` (what the rest of the file calls it) or a `name` (what people see
in the web page), a pin, and a default MIDI message. **What a control sends
can be changed later from the web page** without rebuilding. **How it's
wired can't**: that's the YAML.

The format follows ESPHome's conventions: pins are a name like `PA1` or
`GPIO5`, or a mapping such as `{ number: PA1, inverted: true }`; times carry
a unit (`125us`, `3ms`); ids are `lower_snake_case`. The builder checks the
file and reports every mistake with its line number.

This page is an overview. The full reference is
[`core/README.md`]({{ site.blob }}core/README.md), and the costs below come
from [`core/performance.md`]({{ site.blob }}core/performance.md), where each
figure says whether it was measured on hardware or worked out from the code.
The web page's **YAML examples** panel (in the dashboard) has copyable
examples of the newer parts.

<h2 id="basics">The basics</h2>

{% raw %}
```yaml
gemifi: 2                      # format version
name: keystation-mini32        # node name: names the built images
friendly_name: Keystation Mini 32   # what the computer and the web page show
project: { version: 0.1.9 }
board_id: 0x0001               # 0x3F00-0x3FFF are free for DIY builds
platform: nuvoton-nuc1xx       # nuvoton-nuc1xx | stm32f1 | esp32 | esp32s2 | esp32s3 | esp8266
chip: NUC120LC1BN              # ESP: the PlatformIO board id
usb: { vid: 0x1209, pid: 0x0002 }
scan_interval: 125us           # how often keys and encoders are scanned
boot_combo: [pitch_down, pitch_up]
```
{% endraw %}

- `midi: { transport: usb | serial | ble }` picks how an ESP board talks
  MIDI. The STM32F1 and NUC1xx always use USB. See [Platforms]({{ '/platforms/' | url }}).
- `boot_combo` names buttons that, held at power-up, start the app with
  default settings (safe mode), or, on a NUC1xx keyboard, keep it in the
  gemifi bootloader.
- `scan_interval` trades CPU time for timing detail: a shorter tick gives
  finer key velocity and faster encoders, a longer one leaves more time for
  everything else.

<h2 id="keys">Keys</h2>

One `key_matrix` per device. `type: switch` keys have one contact and play at
a fixed velocity. `type: velocity` keys have two contacts each, and the time
between them gives the velocity: `velocity: { fast, slow }` set the times
that mean 127 and 1. Rows can be MCU pins, 74HC595 outputs or decoder
outputs; columns are MCU pins or 74HC165 inputs. `key_map` handles a keybed
whose wiring doesn't follow the default order. Put diodes on the matrix if
more than one key can be held.

{% raw %}
```yaml
key_matrix:
  type: velocity
  rows: [PA8, PA9, PC0, PC1, PC2, PC3, PC6, PC7]
  columns: [PB0, PB1, PB2, PB3, PB4, PB5, PB8, PB12]
  keys: 32
  first_note: 48
  velocity: { fast: 1.6ms, slow: 120ms }
```
{% endraw %}

**Software scan** (every platform). Each tick drives one row at a time and
reads the columns. With rows and columns on MCU ports, a fast path skips rows
that haven't changed: the Keystation's 8 rows take about 21 µs of each
125 µs tick. Velocity comes in steps of one tick, so a hard key strike (2–4 ms
between contacts) gets 16–32 steps at 125 µs. A note goes out on the next
main-loop pass, then USB takes up to 1 ms.

**Hardware scan** (STM32F1 only). With `hardware_scan:`, timers and DMA scan
the whole matrix row by row and the CPU only looks at finished frames. Key
times are measured per frame: 36 µs on the Code 61 with `precharge`, 84 µs
without. It needs every row selected by one register write and every column
on one input port; a device that doesn't fit logs a warning and scans in
software. `precharge` drives the columns between rows, which needs a matrix
with diodes or tolerant drivers, so leave it off on a new board until tried.
`analog: true` does the same for faders and pads (below).

{% raw %}
```yaml
hardware_scan:
  matrix: { row_time: 1.5us, settle: 1us, precharge: true }
  analog: true
```
{% endraw %}

<h2 id="buttons">Buttons, modes and layers</h2>

A button has a pin, an `action`, its number (`cc:` or `note:`) and
optionally an `led:`. The actions: `cc_momentary`, `cc_toggle`, `cc_ramp`
(glides to 127 while held), `note`, `octave_down` / `octave_up`,
`transpose_down` / `transpose_up`, `bend_down` / `bend_up`, `program_next` /
`program_prev`, `preset_next` / `preset_prev` / `preset_select`, `mcu`
(Mackie Control), and `none`. The [MIDI message](#messages) actions add
`program` (with an optional bank), `panic`, `mmc` (play, stop, record, ...)
and `sysex`.

{% raw %}
```yaml
buttons:
  - { id: sustain, pin: { number: PA1, inverted: true }, action: cc_momentary, cc: 64, led: sustain_led }
  - { id: oct_down, name: Oct -, pin: { number: PA3, inverted: true }, action: octave_down }
```
{% endraw %}

- **Limits and cost.** Up to 64 buttons. A button on an MCU pin is read
  every millisecond and a release counts after 10 ms open. On a switch
  matrix it's seen at the matrix's next scan; on an I2C expander, up to 1 ms
  later.

**Modes** turn the keys and buttons into function keys while a button is
toggled (`toggle:`) or held (`hold:`). A mode's `keys:` bind keys to actions
such as picking a preset or a MIDI channel, its `buttons:` give buttons
other actions, and sub-modes (`parent:`) take one more key press. The
Keystation's Edit button is a mode: Edit, then E3, then a white key picks a
preset. Modes are the same in every preset, so a preset can't take away the
way back.

**Layers** give buttons, pads and encoders a second meaning while a mode is
on. Each one in the mode's `layer:` gets a *twin*: its own CC or note, toggle
state, LED look and DAW role, on the same switch. `with:` makes a mode a
Shift combination, and a **cycle** steps through up to 8 layers with one
button, showing each one's colour on the button's LED and its name on the
display.

{% raw %}
```yaml
modes:
  - id: arm
    toggle: { button: button_mode, with: shift }   # Shift + Button mode
    layer:
      b2: { cc: 52, daw: arm_1, active: red }      # B2 arms track 1, red while armed
```
{% endraw %}

Twins' settings are global, not per preset. On the Code 61, 24 encoder twins,
14 button twins and 7 modes take 128 bytes of the stored settings.

<h2 id="encoders">Encoders</h2>

A quadrature encoder has `pin_a`, `pin_b`, `steps_per_detent` and a default
`cc`. It sends in one of four modes, changeable from the web page:
absolute (0–127), relative two's complement (1 = +1, 127 = −1), relative
offset (65 = +1, 63 = −1) or relative sign-magnitude (1 = +1, 65 = −1).
`mode:` sets the default. It can also be absolute 14-bit CC, NRPN or RPN
([MIDI messages](#messages)).

`acceleration:` (off by default) makes a quick turn move more than one step
per detent, so a flick sweeps the range and a slow turn stays exact. The
presets are `low`, `medium` and `high`, or set `{ slow, fast, max, curve }`
yourself. `encoder_settings:` sets the default for every encoder. It's fixed
at build time for now.

{% raw %}
```yaml
encoder_settings: { acceleration: medium }
encoders:
  - { id: encoder_1, pin_a: PB6, pin_b: PB7, cc: 20 }
  - { id: encoder_2, pin_a: PB8, pin_b: PB9, cc: 21, acceleration: off }
```
{% endraw %}

- **Limits and cost.** Each step of the quadrature signal has to last one
  tick, so at `scan_interval: 500us` an encoder manages about 500 detents a
  second at best (worked out, not measured). On an I2C expander it's stepped
  once per millisecond, about 250 detents a second at best and much less in
  practice: put encoders on MCU pins. Encoders can't go on a multiplexer.
- With `mcu:` an encoder is a Mackie Control V-Pot, and `ring:` lists the
  LEDs of its ring.

<h2 id="analog">Faders, knobs and wheels</h2>

An `analog:` input is a pot, fader, wheel, expression pedal or pressure
strip on an ADC pin or a multiplexer channel. `adc_min` and `adc_max` are
the raw readings (0–4095) at each end; swap them to reverse it.

{% raw %}
```yaml
analog:
  - { id: volume, pin: PA0, cc: 7, adc_min: 32, adc_max: 4064, takeover: pickup }
  - { id: pitch_wheel, name: Pitch, pin: PA4, mode: pitch_bend }
  - { id: aftertouch, pin: PC3, mode: channel_pressure, adc_min: 2304, adc_max: 1232 }
```
{% endraw %}

- **Modes.** `cc` (the default), `pitch_bend` (14-bit; the reading at
  power-up is the centre, so leave a sprung wheel alone while the device
  starts) or `channel_pressure` (aftertouch). Also `cc14`, `nrpn`, `nrpn14`,
  `rpn`, `rpn14` and `sysex` ([MIDI messages](#messages)).
- **Pick-up.** `takeover: pickup` keeps a knob silent after the DAW sets its
  value, until the knob reaches it, so values don't jump.
- **Touch.** `touch: movement` tells a DAW script when a fader starts and
  stops moving.
- **XY pads.** A 4-wire resistive pad is an `xy_pad:` with two axes, `x` and
  `y`, used as analog inputs. It's read in software, one axis per
  millisecond, and a touch counts after about 4 ms. An axis can't be pitch
  bend.
- **Speed.** Readings are smoothed, so a fader slammed end to end needs about
  22 readings to send its last value. In software, CC inputs take turns, one
  reading every 2 ms between them: one input takes about 44 ms, 8 faders on
  one multiplexer about 350 ms. Wheels and pressure are read every 2 ms each.
  With `hardware_scan.analog` (STM32F1) every input is read every
  millisecond: about 22 ms. These are worked out from the code.

<h2 id="messages">MIDI messages</h2>

Beyond notes and 7-bit CCs, a control can send:

- **14-bit CC** (`mode: cc14`): the MSB on the CC, the LSB on CC + 32. A
  fader then sends about 1300 steps over its travel instead of 128.
- **NRPN and RPN**, 7- or 14-bit (`mode: nrpn14, nrpn: 1234`), optionally
  followed by the null RPN.
- **Bank select** before a program change, on a `program` button or in a
  preset (`program: 5, bank: 300`).
- **Panic**: Note Off for everything gemifi holds, then All Sound Off, All
  Notes Off and Reset All Controllers on all 16 channels.
- **MMC** transport commands: `action: mmc, mmc: play`.
- **SysEx strings** written in the YAML, with the value, its two 7-bit
  halves, the channel and a Roland checksum filled in. This is how older
  hardware synths are controlled.

{% raw %}
```yaml
midi_messages:
  sysex:
    - { id: gs_volume, data: "F0 41 10 42 12 [40 00 04 v] cs F7" }
buttons:
  - { id: panic, pin: PB2, action: panic }
  - { id: play, pin: PB1, action: mmc, mmc: play }
analog:
  - { id: fader_1, pin: PA0, mode: cc14, cc: 7 }
  - { id: volume, pin: PA1, mode: sysex, sysex: gs_volume, min: 0, max: 100 }
```
{% endraw %}

- **Channel and range.** `channel:`, `min:` and `max:` on a button, fader or
  encoder send on its own channel and over its own range (swap min and max
  to invert). They're set at build time, the same in every preset.
- **Cost.** Each kind is compiled in only when the YAML uses it (or lists it
  in `midi_messages.enable`, to pick it later from the web page). With all
  of them on, the Keystation's firmware grows by about 1.1 KB and the Code
  61's by 1.7 KB. A device that uses none gets the same firmware as before.

<h2 id="pads">Pads</h2>

Pressure pads (FSR, like drum pads) play a note with a velocity from how hard
they're hit. Each pad is an ADC pin or a multiplexer channel, with a default
`note` and optionally an LED that lights while it's held. What all pads
share goes in `pad_settings:`: channel, velocity curve, poly aftertouch, and
the sensing thresholds, which are live settings you can tune from the web
page.

{% raw %}
```yaml
pads:
  - { id: kick, pin: { cd74hc4051: pad_mux, number: 0 }, note: 36 }
  - { id: snare, pin: { cd74hc4051: pad_mux, number: 1 }, note: 38 }
pad_settings: { channel: 10, threshold: 200, full: 3000, window: 3ms }
```
{% endraw %}

- **Latency.** A hit is seen within 1 ms of crossing `threshold`; its peak is
  then searched for `window` (3 ms by default) before the Note On goes out.
  A shorter window answers sooner, a longer one finds the peak more reliably.
- **Cost.** Every pad is read every millisecond: 16 pads in software take
  about 0.15 ms of each millisecond on an STM32F1. With `hardware_scan.analog`
  the hardware keeps each pad's peak between passes.
- **Aftertouch** sends poly pressure at most every 10 ms per pad. 16 pads
  held send about 4.8 KB/s, more than DIN MIDI carries.

<h2 id="leds">LEDs</h2>

An LED is one pin, two (`pin2:`, bicolour) or three (`pin3:`, RGB). It
shows its button's state, the octave or transpose, a pad being held, or what
the DAW sends. `idle:` and `active:` choose what it looks like when off and
on, so a panel can be lit all the time and still show presses;
`led_defaults:` sets that for every LED.

{% raw %}
```yaml
led_defaults: { idle: blue, active: white }
leds:
  - { id: sustain_led, pin: { number: PA12, inverted: true } }
  - { id: b2_led, pin: PB0, pin2: PB1, pin3: PB2, idle: white, active: blue }
```
{% endraw %}

An LED can sit on an MCU pin, a 74HC595, an MCP23017 or PCF8574, a
**TM1628**-family LED driver (and relatives with the same commands), or an
**`rgb_serial`** LED board (for now only the Code 61's pad LED board, on the
STM32F1). Levels are worked out every 10 ms and only changes are written:

| Where | Cost |
|---|---|
| MCU pin | one write when it changes |
| 74HC595 | the chain is shifted out in the next scan tick |
| MCP23017 / PCF8574 | one I2C write when it changes, about 80 / 50 µs |
| TM1628 | at most 9 bus bytes per main-loop pass, about 110 µs; one LED about 40 µs (STM32F1) |
| `rgb_serial` | one LED every 5 ms: 16 pads all changing take 80 ms |

**Ownership.** When both the device and the DAW want an LED, `owner:`
decides: `host_then_local` (the default) shows the DAW's state for 2 s after
its last message, then the button's again; `host` shows only the DAW's;
`local` ignores the DAW. LEDs driven by Mackie Control default to `host`.

To find which output lights which LED on new hardware, build with an LED
walk: each listed LED lights on its own in turn, and the log names it.

<h2 id="displays">Displays</h2>

A 14-segment character display, like the Code 61's six characters, is a row
of characters on a hub's outputs, usually a TM1628. It shows "GEMIFI" at
boot, then the last message the device sent (`74 127` for a CC, `C#4.127`
for a note), a preset's name when you switch, and `CH 3` when the channel
changes. A DAW script can write its own text to it (`daw:` names what it
shows: `time`, `track_name`, `param` or `status`). One display per device,
up to 16 characters.

{% raw %}
```yaml
displays:
  - id: display
    type: segment14
    daw: param
    chars:
      - { tm1628: leds_a, number: 48 }
      - { tm1628: leds_a, number: 64 }
```
{% endraw %}

<h2 id="hubs">Expander chips</h2>

When the MCU runs out of pins, a hub adds more. Each chip type is its own
list, named as in ESPHome, and a pin on it names the hub:
`{ sn74hc595: leds, number: 3 }`. Hubs that share a control line (one clock
for a 74HC595 and a 74HC165, say) mark it `allow_other_uses: true` on every
use.

| Hub | Gives | Cost |
|---|---|---|
| `sn74hc595` | 8 outputs per chip, 1–4 chips: LEDs, matrix rows | written in the scan tick only when an output changed |
| `sn74hc165` | 8 inputs per chip, 1–4 chips: buttons, matrix columns | read every tick, about 4–8 µs per chip (#70) |
| `cd74hc4051` / `cd74hc4067` | 8 / 16 analog or digital inputs | waits `delay` (default 2 µs) each time it switches channel |
| `mcp23017` | 16 GPIO over I2C | read every 1 ms if it has inputs: about 120–150 µs at 400 kHz (#72) |
| `pcf8574` | 8 GPIO over I2C | read every 1 ms if it has inputs: about 50 µs |
| `decoder` | 74HC138-style row selects for a matrix | one register write per row on STM32F1 |
| `switch_matrix` | up to 32 buttons or encoder contacts on a matrix | scanned with the key matrix |
| `tm1628` | LED driver outputs (LEDs, displays) | see LEDs |
| `rgb_serial` | RGB LEDs on a serial LED board | see LEDs |
| `xy_pad` | the two axes of an XY pad | see Faders |

Inputs on the shift registers and I2C chips are read into a cache (74HC165s
every scan tick, I2C chips every millisecond), so a whole key matrix can live
on shift registers, but MCU pins are much faster. An I2C chip with inputs costs a good share of every main-loop
pass, so put inputs on as few of them as you can.

<h2 id="din">DIN MIDI</h2>

The 5-pin MIDI jacks (STM32F1 only so far: USART1, TX PA9, RX PA10).
`mode:` decides how they reach the computer:

- `usb_merge` (the default): MIDI In arrives on the device's own USB port,
  mixed with its keys; with `usb_to_din` the computer can also play MIDI Out.
- `separate_port`: a second USB-MIDI port for the jacks. On Windows a device
  with two USB-MIDI ports can hang Chrome's Web MIDI, and with it the web
  updater (#54).
- `din_only`: the jacks don't reach USB.

`midi.din_out` (the device's own messages also go to MIDI Out, on by
default) and `midi.thru` (MIDI In copied to MIDI Out, off) are settings.
DIN carries about 3 bytes a millisecond, so gemifi uses running status and,
when the queue is long, sends only the latest pitch bend, pressure or CC
value, so wheels and faders can't build up a backlog.

{% raw %}
```yaml
din_midi: { tx_pin: PA9, rx_pin: PA10, mode: usb_merge }
```
{% endraw %}

<h2 id="presets">Settings and presets</h2>

Every control's message (a button's action and number, a fader's CC and
mode, an encoder's CC and mode, a pad's note) and the global settings (MIDI
channel, transpose, octave, velocity curve, glide and ramp times) are
**settings**: the web page reads and changes them, and **Save to device**
writes them to flash.

**Presets** are named sets of those settings. The YAML lists them (each
one only says what differs from the first), and you switch with preset
buttons, a mode, or the web page, where you can also make, rename, move and
delete them. The builder checks that the worst case fits the device's
settings store: the Keystation has 8 slots (up to 9 fit), the Code 61 has 4
(up to 5 fit).

{% raw %}
```yaml
presets:
  slots: 8
  list:
    - name: Default
    - name: Drums
      midi: { channel: 10 }
      buttons: { sustain: { action: note, note: 36 } }
```
{% endraw %}

**Writes the device makes on its own.** A flash write can stop the CPU, and
the key scan with it, for 20–40 ms, so by default the device only writes
when a host asks. `write_to_flash: { last_preset: true }` makes it remember
the last preset, written only once you've left it alone (10 s after the
switch and 3 s with nothing played, by default).

<h2 id="feedback">DAW feedback</h2>

What the DAW sends back can drive the device. There are three ways, and
[DAW setup]({{ '/daw-setup/' | url }}) has the steps for each DAW:

- **Plain feedback (F1)**, on by default, for any host that sends MIDI back:
  a toggle button's CC sets its state, a note or CC lights an LED
  (`feedback: { note: 40 }`), LEDs can follow the MIDI clock beat or the
  transport, absolute encoders take the host's value, and `takeover: pickup`
  stops knobs from jumping. On the Keystation it costs about 0.5 KB of flash
  and 24 bytes of RAM.
- **Mackie Control (F2)** with `mcu:`: the device gets a second USB-MIDI port,
  "&lt;name&gt; DAW", for transport, mute/solo/arm/select, faders as pitch bend,
  V-Pots and their rings. About 0.5 KB more.
- **DAW scripts (F3)**, on by default: a script in the DAW asks the device
  for its controls and connects them by their `daw:` role (`play`,
  `volume_1`, `device_param_1`, ...), with no MIDI mapping. About 1.5 KB of
  flash on the Keystation. The scripts are for Ableton Live, Bitwig and
  Zynthian; the roles are listed in
  [`hosts/README.md`]({{ site.blob }}hosts/README.md).

{% raw %}
```yaml
buttons:
  - { id: rec, pin: PB0, action: cc_momentary, cc: 20, led: rec_led, daw: record }
leds:
  - { id: beat_led, pin: PC5, feedback: { clock: beat } }
```
{% endraw %}

The messages are in
[`protocol/host-feedback.md`]({{ site.blob }}protocol/host-feedback.md) and,
for script writers,
[`protocol/daw-scripts.md`]({{ site.blob }}protocol/daw-scripts.md).

</div>

<p class="readme-link">
  The full YAML reference is <a href="{{ site.blob }}core/README.md" rel="noopener"><code>core/README.md</code></a>,
  the costs are in <a href="{{ site.blob }}core/performance.md" rel="noopener"><code>core/performance.md</code></a>,
  and the builder's checks are in <a href="{{ site.blob }}tools/builder/README.md" rel="noopener"><code>tools/builder/README.md</code></a>.
  Every device in <a href="{{ site.tree }}devices" rel="noopener"><code>devices/</code></a> is a worked example.
</p>
