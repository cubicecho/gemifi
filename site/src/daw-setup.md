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
the host only; the DAW checks are tracked in
[#26]({{ site.repo }}/issues/26). Menu names below are from each DAW's
documentation; where we couldn't check one, it says so.

## Three ways to connect

| | Port | Works with | You get |
|---|---|---|---|
| **Plain feedback** | the device's main port | any host that sends MIDI back | LEDs follow mapped notes and CCs, toggles and encoders stay in sync, knobs don't jump (`takeover: pickup`), beat and play LEDs from MIDI clock |
| **Mackie Control** | the "`<name>` DAW" port | DAWs with a Mackie Control surface | transport, mute/solo/arm/select buttons with LEDs, faders, V-Pots and their rings |
| **gemifi script** | the main port | DAWs we have a script for | the device's controls connected by their `daw:` roles, no MIDI mapping needed |

The device has a DAW port only when its YAML has `mcu:` (and it's on USB).
Plain feedback and the script are on by default. The exact messages are in
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
- **gemifi extension:** in progress in
  [`hosts/bitwig/`]({{ site.tree }}hosts/bitwig) (see its README once it's
  merged). Not released yet.
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
(`zyngine/ctrldev/` in zynthian-ui), and has one for Mackie Control. Whether
it works with a gemifi DAW port, and how to select it, is still to check.

A gemifi driver could use the gemifi script protocol the way the Live script
does: learn the controls from the device and drive its LEDs. None exists yet.

## What to report

If you try one of these, open an issue with the DAW and its version, the
device and its YAML, and what did or didn't light up.

</div>

<p class="readme-link">
  Back to <a href="{{ '/get-started/' | url }}">Get started</a>.
</p>
