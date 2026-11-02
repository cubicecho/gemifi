---
title: Code 49 / 61
description: M-Audio Code 49 and Code 61 with gemifi — what we know about the hardware and stock firmware, and what's still open.
maker: M-Audio
model: Code 49 / Code 61
status: researched
order: 3
summary: Keyboard controllers with pads, faders and encoders on an STM32F103. Researched from the official firmware update; no gemifi build yet.
facts:
  - [MCU, "STM32F103, high-density (inferred from the firmware; not yet confirmed on a board)"]
  - [Core, "Arm Cortex-M3, 72 MHz"]
  - [Memory, "Exact part and flash size unconfirmed"]
  - [Controls, "Keys with velocity and aftertouch, pads, faders, encoders, pitch and mod wheels. Counts not documented here yet."]
  - ["USB (gemifi)", "Not assigned yet"]
  - [gemifi platform, "None yet (an STM32F1 platform would be new)"]
yaml: []
photos: We haven't had one open yet, so the chip marking and any SWD header are unconfirmed.
---

## What we know

Everything here comes from studying the official **Code 49 v1.2.1** firmware
updater, without running it. The Code 25, 49 and 61 ship the same firmware
version on the same dates, so the Code 61 is very likely the same code with a
different key count. That is not confirmed.

- **Chip:** the firmware is built for an STM32F103 high-density part (a
  Cortex-M3), judging by its interrupt table, peripherals and memory layout.
  Nobody has read the marking on a real board yet.
- **Layout:** a stock bootloader at the start of flash, which an update leaves
  in place, then the app. Settings and per-unit calibration live in a separate
  area of flash that updates don't touch.
- **No encryption:** the update carries the app as plain data with a simple
  checksum.
- **Updates are plain MIDI SysEx.** The updater needs no special driver, so in
  principle a browser page with Web MIDI can do what it does.

## Why it's interesting

The Code's pads, faders and encoders feel “mushy”: slow to respond and hard to
play precisely. Reading the firmware, there's no single bug. Each input is
dulled by its tuning, such as
pads that need nearly full force for top velocity, a large dead zone on idle
faders, and encoder acceleration that's on by default. Those are exactly the
values gemifi makes adjustable from the web page.

## How gemifi would get on it

**Under investigation.** Two routes are open:

- **Through the stock update mechanism**, which would mean no case opening and
  no SWD. We don't know yet how the keyboard enters update mode (the official
  updater just says to turn it off and on), or what the stock bootloader checks
  before starting an app.
- **Over SWD**, if the board has a usable debug header. Not known until
  someone opens one.

Either way the stock bootloader can probably stay as a recovery path, since
updates don't replace it.

## Back to stock

Not worked out yet. If installing goes through the stock update mechanism,
re-running M-Audio's own updater would be the obvious way back, but that hasn't
been tried.

## Risks

- Nothing has been flashed or tested on hardware. Treat everything above as
  research notes, not instructions.
- Settings and calibration are per unit. Anything that erases that area loses
  your unit's calibration unless you back it up first.
