---
title: Keystation Mini 32 MK3
description: M-Audio Keystation Mini 32 MK3 with gemifi — status, hardware, SWD test points, how to install and how to go back to stock.
maker: M-Audio
model: Keystation Mini 32 MK3
status: works
order: 1
summary: A 32-key USB MIDI keyboard on a Nuvoton NUC120. The first device gemifi runs on, end to end.
restore: true
facts:
  - [MCU, "Nuvoton NUC120LC1BN (LQFP48)"]
  - [Core, "Arm Cortex-M0, rated 50 MHz; runs at 48 MHz from a 12 MHz crystal"]
  - [Memory, "32 KB app flash (APROM), 4 KB boot flash (LDROM), 4 KB data flash, 4 KB SRAM"]
  - [Controls, "32 keys with velocity, 7 buttons, 4 LEDs (2 bicolour), 1 knob"]
  - ["USB (stock)", "<code>0763:3111</code>, class-compliant USB MIDI"]
  - ["USB (gemifi app)", "<code>1209:0002</code> “gemifi (Keystation Mini 32)”"]
  - ["USB (gemifi bootloader)", "<code>1209:0001</code> “gemifi boot (Keystation Mini 32)”"]
  - [gemifi platform, "<a href=\"../../platforms/#nuvoton-nuc1xx\"><code>nuvoton-nuc1xx</code></a>, board id <code>0x0001</code>"]
yaml:
  - devices/keystation-mini32-mk3.yaml
photos: Our test unit was photographed during bring-up, but those photos aren't on this site yet.
---

## What works

Checked on our unit on 2026-10-31, with the gemifi bootloader in LDROM and the
app installed over USB:

- all 32 keys, with velocity from the time between each key's two contacts
- all 7 buttons and their MIDI messages, the volume knob over its full range,
  and all LEDs, including the octave levels on the bicolour LEDs
- reading and changing settings from the web page, and saving them to the device
- installing a new app over USB (about 4 seconds), and rebooting between the
  app and the bootloader

Default mapping (every button's action and number can be changed from the web page):

| Control | Sends |
|---|---|
| 32 keys | Notes from 48 (C3), with velocity |
| Sustain | CC 64 while held |
| Edit | Edit mode: the keys pick presets (E3, then a white key from D4), channels (A3, then a key from C4), octave and velocity curve, until Edit is pressed again |
| Oct − / Oct + | Shift the octave −4 to +4; both together go back to 0 |
| Pitch ▼ / ▲ | Pitch bend to the end with an 80 ms glide, back on release |
| Mod | CC 1, ramps up to 127 over 300 ms while held |
| Volume knob | CC 7 |

**Merged but not yet checked on hardware** ([#7]({{ site.repo }}/issues/7)):
the bootloader's boot-loop guard and stored serial number, presets and the
Edit mode below, DAW feedback and the optional DAW port, and the USB name
“gemifi (Keystation Mini 32)”. The boot-loop guard only sees restarts the app
makes itself; an app that hangs isn't caught. Holding Pitch ▼ + Pitch ▲ at
power-up always gets you into the bootloader.

## The board

- **U1** is the NUC120LC1BN, a part Nuvoton has since made obsolete.
- **X1** is an HC-49 crystal. Its marking isn't readable; 12 MHz is the only
  value that gives USB its 48 MHz clock.
- **AR1–AR6**, resistor arrays near the keybed ribbon connector, are the
  pull-ups and pull-downs for the buttons and key matrix. The NUC120 has no
  internal pull resistors.
- **TP1–TP4**, four through-hole test points in a row at the top right, are a
  ready-made SWD header:

| Test point | Signal | U1 pin |
|---|---|---|
| TP1 | ICE_DAT (SWDIO) | 29 |
| TP2 | ICE_CK (SWCLK) | 30 |
| TP3 | VDD (about 5 V) | 11 |
| TP4 | GND | 12, 31 |

The chip's /RESET is U1 pin 46. We haven't found a pad for it yet; it's only
needed if a broken app ever stops SWD from connecting.

We haven't written up how to take the case apart yet.

## Keys, buttons, LEDs and knob

The full pin map is the device YAML. In short:

- **Key matrix:** 8 drive rows (PA8, PA9, PC0–PC3, PC6, PC7, driven high one at
  a time) by 8 sense columns (PB0–PB5, PB8, PB12, with external pull-downs).
  Each key has two contacts, so 64 contacts make 32 keys. gemifi scans it every
  125 µs, which takes about 21 µs of each tick.
- **Buttons:** PA1–PA7, active-low, not part of the matrix: Sustain, Edit,
  Oct −, Oct +, Pitch ▼, Pitch ▲, Mod.
- **LEDs:** active-low. Sustain on PA12, Edit on PA13, Oct − on PA10 and PA14,
  Oct + on PA11 and PA15 (two colours each). We haven't confirmed which colours.
- **Volume knob:** PA0, the chip's ADC channel 0.
- PB15 is the only GPIO the board doesn't use.

## How gemifi gets on it

SWD is needed once, to put the gemifi bootloader in LDROM. After that, every
update goes over USB from the browser.

1. **Wire up SWD.** TP1 to SWDIO, TP2 to SWCLK, TP4 to GND. Leave TP3 and the
   probe's power pins unconnected: the keyboard powers itself and will
   back-power the probe otherwise. Plug in the probe first and the keyboard
   second; unplug the keyboard first. We used a Bus Pirate v3 with OpenOCD,
   which works but is slow; a CMSIS-DAP probe such as a Raspberry Pi Pico
   running debugprobe is faster.
2. **Back up the stock firmware** with `tools/swd/nuvoton-nuc1xx/dump.sh`. It
   reads app flash, boot flash, data flash and the config words twice and only
   keeps them if both reads match. Our unit's chip wasn't read-protected. If
   yours is, stop: unlocking it erases the stock firmware.
3. **Flash the bootloader** with `flash-ldrom.sh`, which refuses to run without
   a verified backup. The config words are never written: the chip already
   boots from LDROM.
4. **Install the app.** With no app on it, the keyboard starts in update mode
   (the octave LEDs blink in turn). Open the web updater, connect, and flash the
   `.gfw` image you built.
5. **Later updates** go the same way. The page reboots the app into the
   bootloader by itself. To force update mode, hold Pitch ▼ + Pitch ▲ while
   plugging the keyboard in.

The step-by-step runbook, with every command, is
[`tools/README.md`]({{ site.blob }}tools/README.md); the bootloader is described in
[`bootloader/nuvoton-nuc1xx/README.md`]({{ site.blob }}bootloader/nuvoton-nuc1xx/README.md).

## Back to stock

`tools/swd/nuvoton-nuc1xx/restore-stock.sh <backup>` writes your backed-up app
and boot flash back over SWD. Add `--with-dataflash` to restore the stock
settings too: the first time gemifi saves settings, it replaces the stock ones
in data flash.

This needs **your own backup**. gemifi doesn't ship or host stock firmware.
SWD is the way back.

## Risks

- **No backup, no way back.** Without a backup of your unit there is no stock
  firmware to restore.
- **Opening the case and wiring SWD.** You'll be clipping or soldering to test
  points on a powered board.
- **Back-powering.** Connecting the probe's power to TP3, or unplugging in the
  wrong order, feeds the keyboard's 5 V into the probe.
- **Matrix lines.** The rows are push-pull outputs. Firmware that drives rows
  and columns against each other could damage the chip, so use the published
  device YAML rather than inventing a pin map.
- **A read-protected chip** can only be unlocked by erasing it, which destroys
  the stock firmware.
