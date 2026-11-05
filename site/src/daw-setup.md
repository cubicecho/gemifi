---
layout: base.njk
title: DAW setup
description: Connect a gemifi device to Ableton Live, Bitwig Studio, REAPER, Logic Pro or Zynthian, with LED feedback, Mackie Control or the gemifi script.
permalink: /daw-setup/
---
<section class="hero">
  <h1>DAW setup</h1>
  <p>Which port to pick in each DAW, and what you get.</p>
</section>

<div class="prose">

**None of this has been tried in a DAW yet.** The firmware side is tested on
the host, and the Code 61 has run the web updater's host-feedback demo (LEDs
and display driven the way a script drives them). The DAW checks are tracked
in [#7]({{ site.repo }}/issues/7) (Live), [#18]({{ site.repo }}/issues/18)
(Mackie Control) and [#21]({{ site.repo }}/issues/21) (the scripts). Menu names
below are from each DAW's documentation; where we couldn't check one, it says so.

## Three ways to connect

| | Port | Works with | You get |
|---|---|---|---|
| **Plain feedback** (F1) | the device's main port | any host that sends MIDI back | LEDs follow mapped notes and CCs, toggles and encoders stay in sync, knobs don't jump (`takeover: pickup`), beat and play LEDs from MIDI clock |
| **Mackie Control** (F2) | the "`<name>` DAW" port | DAWs with a Mackie Control surface | transport, mute/solo/arm/select buttons with LEDs, faders, V-Pots and their rings |
| **gemifi script** (F3) | the main port | Ableton Live, Bitwig, Zynthian | the device's controls connected by their `daw:` roles, no MIDI mapping needed |

The device has a DAW port only when its YAML has `mcu:` (and it's on USB).
On Windows, a device with two USB-MIDI ports can hang Chrome's Web MIDI, and
with it the web updater ([#54]({{ site.repo }}/issues/54)); flash such a build
with `gemifi.py` there, or expect it. Plain feedback and the script are on by
default. What each one needs in the YAML: [Components]({{ '/components/' | url }}#feedback). The exact messages are in
[`protocol/host-feedback.md`]({{ site.blob }}protocol/host-feedback.md); the YAML
is in [`core/README.md`]({{ site.blob }}core/README.md#daw-feedback).

Use one of plain feedback or the script on the main port, not both: they'd
both drive the same LEDs.

## Ableton Live

Settings (Live 12; Preferences in Live 11) → **Link, Tempo & MIDI**.

- **Plain feedback:** in the MIDI Ports list, turn on **Remote** for the
  device's input (to MIDI-map it) and its **output** (so Live sends mapped
  values back). For the beat and play LEDs, also turn on **Sync** for the
  output.
- **Mackie Control:** in a Control Surface slot pick **MackieControl**, with
  Input and Output both set to the "`<name>` DAW" port. Live asks the device
  for its firmware version, which gemifi answers.
- **gemifi script:** copy the script into Live's User Library and pick
  **gemifi** as a Control Surface, with Input and Output set to the device's
  **main** port. Install steps:
  [`hosts/ableton-live/README.md`]({{ site.blob }}hosts/ableton-live/README.md).
  Not yet tried in Live ([#21]({{ site.repo }}/issues/21)).

## Bitwig Studio

- **Mackie Control:** Settings → Controllers → **Add Controller**, vendor
  **Mackie**, model **Mackie Control**; set its MIDI input and output to the
  "`<name>` DAW" port. (Vendor and model names are from Bitwig's
  supported-controller list; check Bitwig's manual if your version differs.)
- **gemifi extension:** build it with Maven from
  [`hosts/bitwig/`]({{ site.tree }}hosts/bitwig) and copy
  `gemifi.bwextension` into Bitwig's Extensions folder. Then Settings →
  Controllers → **Add controller**, vendor **gemifi**, model **gemifi**, with
  its input and output set to the device's **main** port (not the DAW port).
  Steps: [`hosts/bitwig/README.md`]({{ site.blob }}hosts/bitwig/README.md).
  Unit-tested, not yet loaded into Bitwig.
- **Plain feedback:** whether Bitwig's generic controllers send mapped
  values back to the device is to check (see Bitwig's manual).

## REAPER

- **Mackie Control:** Preferences → **Control/OSC/web** → **Add**, Control
  surface mode **Mackie Control Universal**, MIDI input and output = the
  "`<name>` DAW" port.
- **Plain feedback:** [ReaLearn](https://www.helgoboss.org/projects/realearn/) (a free
  plugin, installed with ReaPack) sends feedback to an output port: set its
  output to the device's main port. Whether REAPER's own MIDI learn can send
  feedback: check REAPER's manual.

## Logic Pro

- **Mackie Control:** Logic Pro → Control Surfaces → **Setup**. Logic detects
  some Mackie devices by itself. If not: **New →
  Install**, pick **Mackie Designs / Mackie Control**, **Add**, then set its
  input and output port to the "`<name>` DAW" port in the Setup window.
  Exact menu paths vary by Logic version: check Apple's
  [Control Surfaces Support Guide](https://support.apple.com/guide/logicpro-css/welcome/mac).
- **Plain feedback:** not covered here; check Logic's manual for controller
  assignments with feedback.
- No gemifi script for Logic.

## Zynthian

Zynthian drives controllers with Python "ctrldev" drivers
(`zyngine/ctrldev/` in zynthian-ui).

- **gemifi driver:** it learns the device's controls and drives its LEDs,
  pick-up and rings from Zynthian's state, like the Live script, with a clip
  grid on the pads, Zynthian's four knobs on encoders (`zynthian.zynpot_1`–`4`,
  the Code 61's ZYN encoder mode) and text on the display. zynthian-ui is GPL,
  so the driver lives in a zynthian-ui fork,
  [`vantreeseba/zynthian-ui`](https://github.com/vantreeseba/zynthian-ui)
  (branch `ui-modernize`), not in gemifi. Unit-tested, not yet confirmed
  working on a Zynthian. Notes:
  [`hosts/zynthian/README.md`]({{ site.blob }}hosts/zynthian/README.md).
- **Mackie Control:** Zynthian has its own Mackie Control driver, which can be
  bound to the "`<name>` DAW" port. Whether it works with a gemifi device is
  still to check.

## What to report

If you try one of these, open an issue with the DAW and its version, the
device and its YAML, and what did or didn't light up.

</div>

<p class="readme-link">
  Back to <a href="{{ '/get-started/' | url }}">Get started</a>.
</p>
