---
title: FCB1010
description: Behringer FCB1010 MIDI foot controller with gemifi — planned as a main-board swap to an ESP32-S3.
maker: Behringer
model: FCB1010
status: planned
order: 4
summary: A MIDI foot controller with 12 switches and 2 expression pedals. The plan is a new ESP32-S3 main board, powered over USB-C.
facts:
  - [MCU, "Stock: an 8051-family P80C32 with an external EPROM. Planned: ESP32-S3"]
  - [Controls, "10 numbered footswitches plus UP and DOWN, 2 expression pedals, a 3-digit display, switch and status LEDs, 2 amp-switching relays"]
  - ["USB (gemifi)", "Not assigned yet"]
  - [gemifi platform, "<code>esp32s3</code> (planned)"]
yaml: []
photos: We haven't documented our own unit yet.
---

## The plan

The stock FCB1010 runs on an 8051-family P80C32 with its firmware in an EPROM.
Rather than write firmware for that board, the plan is a **brain swap**:
a new main board with an ESP32-S3, keeping the original switch boards,
expression pedals, MIDI board, jack board and enclosure. The mains transformer
comes out, and the unit runs from USB-C.

On the new board, gemifi would give the FCB1010 USB MIDI and BLE MIDI next to
its DIN MIDI ports, with every switch, pedal and LED set up from the web page.

This is **design notes only**: no board, no device YAML, nothing tested. The
wiring is pieced together from community projects, since Behringer hasn't
published a schematic. The closest precedent is
[FCBInfinity](https://github.com/mackatack/FCBInfinity), a Teensy-based
replacement main board that was built and worked.

## What's still unknown

- The exact wire order of the switch-board cables, which differs between
  sources and has to be measured.
- Whether the LEDs are common-anode or common-cathode; sources disagree.
- What the 4-pin MIDI board lead carries.

## How gemifi would get on it

By replacing the main board. The new board's ESP32-S3 would be installed and
updated like any [DIY ESP build]({{ '/devices/diy-esp32/' | url }}).

## Back to stock

Keep the original main board. Putting it back restores the stock unit,
including the mains power section, as long as you kept those parts too.

## Risks

- **Mains voltage.** The stock unit has a mains transformer inside. Unplug it
  before opening it, and don't work on the power section live.
- The switch cable pin order must be checked with a meter before connecting a
  new board.
