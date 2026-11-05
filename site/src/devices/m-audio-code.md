---
title: Code 61
description: M-Audio Code 61 with gemifi — what works, the default mapping, the hardware, how gemifi gets on it over SWD, and the risks.
maker: M-Audio
model: Code 61
status: works
order: 2
summary: A 61-key controller with pads, faders and encoders on an STM32F103. gemifi runs everything on it, from keys and pads to the display and the MIDI jacks.
facts:
  - [MCU, "STM32F103RBT6 (confirmed by its marking and its SWD ID)"]
  - [Core, "Arm Cortex-M3, 72 MHz from an 8 MHz crystal"]
  - [Memory, "128 KB flash in 1 KB pages, 20 KB SRAM"]
  - [Controls, "61 keys with velocity, an aftertouch strip, pitch and mod wheels, 9 faders, 8 encoders, 16 pads with RGB LEDs, an XY pad, the panel buttons with their LEDs, a 6-character display, 5-pin MIDI In and Out, a sustain pedal input, a soft power button"]
  - ["USB (gemifi app)", "<code>1209:0002</code> “gemifi (Code 61)”"]
  - ["USB (gemifi bootloader)", "<code>1209:0001</code> “gemifi boot (Code 61)”"]
  - [gemifi platform, "<a href=\"../../platforms/#stm32f1\"><code>stm32f1</code></a>, board id <code>0x0002</code>"]
yaml:
  - devices/m-audio-code61.yaml
  - devices/m-audio-code61-ledmap.yaml
photos: Our unit has been opened, but its photos aren't on this site yet.
---

## What works

Checked on our unit on 2026-11-03, with the gemifi bootloader installed
over SWD and the app installed over USB:

- all 61 keys with velocity, scanned by the chip's timers and DMA every
  36 µs
- the panel buttons, encoders, faders, pitch and mod wheels, and the
  aftertouch strip
- the 16 pads with velocity, and their RGB LEDs
- the XY pad
- the button LEDs, lit while idle and changing colour when active
- the 6-character display: what the keyboard sends, preset names, `CH <n>`
- the rear MIDI In and Out, mixed into the main USB port, with an optional
  thru
- presets on Up / Down, and the MIDI channel with Channel held
- settings from the web page, saved to flash and kept over a power cycle
- the DAW-script display and LED demo from the web page
- app updates over USB, and the power button (hold it 2 s to switch off)

Some values are still untuned: pad sensing and some fader details are first
guesses (`[guess]` in the device YAML). The open checks are tracked in
[#24]({{ site.repo }}/issues/24).

**Not yet checked on hardware:** the record-arm, pad-grid and encoder layers
below, encoder acceleration, a power cut during a save, the DIN jacks by DMA,
and the rear expression pedal jack, which isn't in the YAML yet. A second USB
port for the MIDI jacks (`separate_port`) works, but hangs Chrome's Web MIDI
on Windows ([#54]({{ site.repo }}/issues/54)), so it's off.

## Default mapping

Everything here can be changed from the web page, per preset.

| Control | Sends |
|---|---|
| 61 keys | Notes from 36 (C2), with velocity |
| Aftertouch | Channel pressure |
| Pitch / Mod wheels | Pitch bend / CC 1 |
| Faders 1–8, Master | CC 20–27, CC 7 |
| Encoders E1–E8 | CC 70–77, absolute, with medium acceleration |
| Pads 1–16 | Notes 36–51 on channel 10 |
| XY pad | CC 16 (X) and CC 17 (Y) |
| B1–B9 | CC 85–93 toggles: mute (white, blue while muted) |
| Transport | CC 110–114 (Rewind, Fast forward, Stop, Play, Record) |
| Up / Down | Next / previous preset; with Channel held, the MIDI channel |
| Octave − / + | Shift the keys an octave |

**Shift combinations** (untested on hardware):

- Shift + Button mode: B2–B9 switch to record arm (red while armed) and back.
- Shift + Pad octave −: the pads become a clip grid for a DAW script.
- Shift + Encoder mode: the encoders step through MIDI, ZYN (Zynthian's
  knobs), DEVICE and PAN.

With a DAW script (Ableton Live, Bitwig or Zynthian; see
[DAW setup]({{ '/daw-setup/' | url }})), the transport, faders, encoders and
B1–B9 connect by their roles, with no MIDI mapping, and the display shows the
control being moved.

## The board

- The **SWD header is JMP1** on the main board, pin 1 the square pad: 1 GND,
  2 SWDIO, 3 SWCLK, 4 3.3 V. There is no reset pin on it.
- The board has a **soft power button**: the firmware holds the power on, so
  a board that's halted over SWD switches itself off unless the power button
  is held (or the probe powers it).
- Keys, panel buttons and encoders share one matrix: 23 rows from decoder
  chips, read on 8 columns. Two LED driver chips drive the button LEDs and the
  display; the pads' LEDs are on a separate pad board fed over a serial line.

The full pin map is the device YAML. The LED-map YAML lights every LED-driver
bit from the web page, one at a time, which is how the LEDs were mapped.

## How gemifi gets on it

**SWD, once.** After that, every update goes over USB from the browser.

1. **Wire up SWD** on JMP1: GND, SWDIO and SWCLK. Leave 3.3 V unconnected
   unless the probe is to power the board, with the keyboard's USB unplugged.
   We used a Bus Pirate with OpenOCD, through
   [`tools/swd/stm32f1/code61.sh`]({{ site.blob }}tools/swd/stm32f1/code61.sh),
   which retries the connection until you press the power button. Keep the
   power button held while it writes.
2. **The chip on our unit was read-protected.** It couldn't be backed up, and
   unlocking it erases the whole chip, including the stock firmware and the
   unit's calibration. There's no way back from that step (see below).
3. **Write the gemifi bootloader**: `code61.sh write-boot` with the build of
   [`bootloader/stm32f1`]({{ site.tree }}bootloader/stm32f1). It sits in the
   first 9 KB of flash and never writes itself.
4. **Install the app.** With no app on it, the keyboard starts in update mode.
   Open the web updater, connect, and flash the `.gfw` image you built from
   the Code 61 YAML.
5. **Later updates** go the same way: the page reboots the app into the
   bootloader by itself. To force update mode, hold **Up + Down** while
   switching on.

A step-by-step guide for the Code 61, like the Keystation's, isn't written
yet. The bootloader's rules are in
[`bootloader/README.md`]({{ site.blob }}bootloader/README.md).

## Back to stock

Not possible with gemifi's tools. Unlocking the chip erases the stock
firmware, and gemifi doesn't ship or host it.

## Risks

- **No way back.** Read the step about read protection before you start.
- **Opening the case and wiring SWD** on a powered board.
- **Calibration.** The unit's own calibration is erased with the chip. gemifi
  calibrates the faders, wheels and aftertouch from values in the YAML
  (`adc_min` / `adc_max`), which may need adjusting for your unit.
- **Not every combination is tried.** Changing the hardware scan settings,
  or turning on DMA for the MIDI jacks, moves the chip's timers around in
  ways not yet checked on hardware.
