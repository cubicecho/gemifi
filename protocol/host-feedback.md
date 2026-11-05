# gemifi host feedback, as implemented

What a gemifi device does with the MIDI a host (a DAW) sends back to it, and what it sends on its DAW port. This describes the firmware as built (`core/src/midi_in.c`, `core/src/controller.c`, `core/src/mcu.c`) and the YAML that turns each part on (`tools/builder/gemifi_device.py`). The YAML itself is documented in `core/README.md` "DAW feedback".

**Status: host-tested (core unit tests, the NUC host-sim), not yet checked on hardware or in a DAW.** Hardware and DAW checks are tracked in #26 (and its children #7, #18, #21). Plan and background: `research/protocols/daw-feedback-plan.md`, `research/protocols/host-feedback.md`, issue #6.

Three layers, each optional:

| Layer | Port | What | Section |
|---|---|---|---|
| F1 plain feedback | main | Notes, CCs, clock and transport from any host drive LEDs, toggles, encoders and pick-up | §2 |
| F2 Mackie Control | DAW port (or the only port) | A subset of the MCU protocol | §3 |
| F3 DAW scripts | main | gemifi SysEx, addressed by component index | §4 (`sysex.md` §4.6) |

The gemifi SysEx protocol (config, presets, firmware) is `protocol/sysex.md` and always runs on the main port.

## 1. Ports and routing

### 1.1 Ports

| Transport | Ports | YAML |
|---|---|---|
| USB, no `mcu:` | one: cable 0, "`<friendly_name>`". The USB descriptor is the same as a build without feedback | – |
| USB with `mcu:` | two: cable 0 "`<friendly_name>`" (main) and cable 1 "`<friendly_name>` DAW", on the same endpoints | `usb: { daw_port: true }`, the default with `mcu:` |
| USB with `mcu:` and `daw_port: false` | one: cable 0 is both the main port and the MCU port | `usb: { daw_port: false }` |
| serial, BLE | one byte stream. With `mcu:` it is the MCU port, and plain feedback is off | – |

`daw_port: true` without `mcu:`, or on serial / BLE, is a build error. (The name uses `friendly_name`, or `name` when there's none.)

### 1.2 Incoming messages

USB-MIDI event packets keep their cable number; serial and BLE give a byte stream, which counts as cable 0.

| Message | Main port (cable 0) | DAW port (cable 1) | Other cables |
|---|---|---|---|
| SysEx | the gemifi protocol (`sysex.md`, incl. F3) | MCU SysEx (§3.5) only | gemifi protocol if the device has no DAW port, else dropped |
| Note On / Off, CC | F1 (§2) | MCU (§3) | dropped |
| Pitch bend | ignored | MCU faders (§3.3) | dropped |
| Channel pressure | ignored | MCU meters (§3.6) | dropped |
| Program change, poly pressure | ignored | ignored | dropped |
| Realtime: clock `F8`, start `FA`, continue `FB`, stop `FC` | F1 (§2.3) | ignored | dropped |
| Other realtime (`FE`, `FF`), system common | ignored | ignored | dropped |

When the only port is also the MCU port (serial, BLE, or `daw_port: false`), every channel message and realtime byte goes to MCU (§3) and F1 is compiled out; SysEx goes to both the gemifi parser and the MCU parser, which ignore each other's messages. The builder refuses LED `feedback:` sources that need F1 (`note`, `cc`, `clock`, `transport`) and `takeover: pickup` on a non-MCU analog in that case, and warns that the host will read every Note On (keys included) as an MCU button.

**Byte stream** (serial, BLE): running status is supported for channel messages. Realtime bytes are taken anywhere, also inside SysEx. A system common byte (`F1`–`F7`) clears running status and is otherwise ignored. Every byte also goes to the gemifi SysEx parser.

**Off switch:** `feedback: { enabled: false }` compiles out F1 and F3; without `mcu:` as well, only the gemifi SysEx is read, as before host feedback existed.

### 1.3 Outgoing

Everything the device sends goes out on the main port, except what an `mcu:` component sends (§3), which goes out on the MCU port. A component with `mcu:` sends only MCU, not its CC or note.

## 2. Plain feedback (F1)

On the main port, with `feedback: { enabled: true }` (the default) and the main port not being the MCU port.

### 2.1 Channels

Note and CC feedback counts only on the device's MIDI channel (the `midi.channel` setting, which a config write or preset can change), with one exception: an LED with `feedback: { note: n, channel: c }` or `{ cc: n, channel: c }` listens on channel `c`. Clock and transport have no channel.

### 2.2 Notes and CCs

Note Off, and Note On with velocity 0, count as value 0. One message can act on several components; each row below is checked.

| Component | Message (device channel unless noted) | Effect |
|---|---|---|
| LED with `feedback: { note: n }` | Note On / Off, note `n` (on `channel:` if given) | LED level from the velocity (§2.4) |
| LED with `feedback: { cc: n }` | CC `n` (on `channel:` if given) | LED level from the value (§2.4) |
| button `action: cc_toggle`, CC `n` | CC `n` | sets the toggle state: value ≥ 64 on, < 64 off. The toggle's LED and its next press follow it (the next press sends the opposite). This is the toggle's own (local) state, so `owner` and the timeout don't apply |
| button `action: cc_momentary` or `cc_ramp` with an LED, CC `n` | CC `n` | the button's LED level from the value (§2.4) |
| button `action: note` with an LED, note `n` | Note On / Off, note `n` | the button's LED level from the velocity (§2.4) |
| encoder, mode `absolute`, CC `n` | CC `n` | becomes the encoder's value: the next turn continues from it. Relative modes ignore it |
| analog input with `takeover: pickup`, CC `n` | CC `n` | becomes its pick-up target (§2.5). Without `takeover: pickup`, ignored |
| key matrix, other button actions | – | no feedback |

An analog input with `mcu:` ignores CCs; it follows pitch bend on the MCU port (§3.3).

### 2.3 Clock and transport

| Message | LEDs with `feedback: { clock: beat }` | LEDs with `feedback: { transport: playing }` |
|---|---|---|
| Clock `F8` | on at the 1st clock of each beat, off at the 7th (lit for the first 16th note; 24 clocks per quarter note) | – |
| Start `FA` | the beat count restarts | on |
| Continue `FB` | the beat count carries on | on |
| Stop `FC` | off | off |

The beat count runs on clock alone, so a host that sends clock while stopped keeps the beat LED blinking.

### 2.4 LED levels

Note velocity and CC value map to a level, as in Mackie Control:

| Value | LED |
|---|---|
| 0 | off |
| 1 | blinking (about 1 Hz: 500 ms on, 500 ms off) |
| 2..127 | on |

On a bicolour LED (`pin2:`), F1 and MCU light colour 1 only (the `pin:`); there are no levels for colour 2 in F1 yet (D2 in the plan). DAW scripts have bicolour levels (`sysex.md` §4.6.2: 2..63 colour 1, 64..127 colour 2). An RGB LED (`pin3:`) is white for any level (or its YAML `active:` look; 0 shows its `idle:` look, core/README.md "LED looks"); only FB_LED_RGB picks its colour. LEDs are redrawn every 10 ms.

### 2.5 Pick-up (`takeover: pickup`)

After the host sets a value (a CC in F1, a fader position in MCU, or FB_VALUE from a script) the knob isn't at, the knob sends nothing until its own 7-bit value reaches or crosses the host's value; from then on it sends normally again. If the knob is already at the host's value, or hasn't been read yet since power-on, nothing is held back. MCU faders have `takeover: pickup` by default (no motors, so the host's position is the target); other analog inputs default to `none`.

### 2.6 LED ownership

An LED can have a **local** state (from the button that uses it: held, toggled, octave, ramp) and a **host** state (the last level the host set, §2.4). `owner:` on the LED, or `feedback.led_owner` for every LED, picks which one shows:

| `owner:` | Shows |
|---|---|
| `host_then_local` (default) | the host state until `timeout` has passed since the host's last message for that LED, then the local state again. `timeout: never` keeps the host state from its first message on |
| `host` | the host state only; off until the host sends something |
| `local` | the local state only; host messages for it are ignored |

- An LED that no button uses has no local state. With `host_then_local` or `host` it shows the host state and doesn't time out. With `owner: local` it stays off.
- `timeout` (on the LED) and `feedback.led_timeout` (the default) take a period from 10 ms to 655 s in 10 ms steps, or `never`. The default is 2 s. A `timeout:` with `owner: host` or `local` is a warning (it isn't used).
- LEDs driven by Mackie Control (an `mcu:` button's LED, `feedback: { mcu: … }`, ring and ring-centre LEDs) default to `owner: host`.
- States set by a DAW script (FB_*) never time out. A later F1 or MCU message for the same LED makes it an ordinary host state again, with the timeout.

## 3. Mackie Control (F2)

With `mcu:` in the YAML. Written from community documentation of the Mackie Control / Logic Control wire format (message formats only; no code from any implementation). "Mackie Control" names only what this is compatible with.

All channel messages below are on MIDI channel 1 unless noted; on other channels they're ignored. The SysEx device id is `0x14` (MCU), or `0x15` (MCU XT) with `mcu: { extender: true }`; SysEx for another id is ignored.

### 3.1 Buttons and LEDs

A button with `mcu: <name>` (`action: mcu`, implied) sends Note On ch 1, note = the name's number, velocity `7F` on press and `00` on release. Its LED, and any LED with `feedback: { mcu: <name> }`, follows the host's Note On for that note: `00` off, `01` blinking, `7F` (anything from `02`) on. Note Off counts as off.

| Notes | Names |
|---|---|
| 0x00–0x07 | `rec_1`–`rec_8` |
| 0x08–0x0F | `solo_1`–`solo_8` |
| 0x10–0x17 | `mute_1`–`mute_8` |
| 0x18–0x1F | `select_1`–`select_8` |
| 0x20–0x27 | `vpot_push_1`–`vpot_push_8` |
| 0x28–0x2D | `track`, `send`, `pan`, `plugin`, `eq`, `instrument` |
| 0x2E–0x31 | `bank_left`, `bank_right`, `channel_left`, `channel_right` |
| 0x32–0x35 | `flip`, `global`, `name_value`, `smpte_beats` |
| 0x36–0x3D | `f1`–`f8` |
| 0x3E–0x45 | `midi_tracks`, `inputs`, `audio_tracks`, `audio_instruments`, `aux`, `busses`, `outputs`, `user` |
| 0x46–0x49 | `shift`, `option`, `control`, `alt` |
| 0x4A–0x4F | `read`, `write`, `trim`, `touch`, `latch`, `group` |
| 0x50–0x53 | `save`, `undo`, `cancel`, `enter` |
| 0x54–0x5A | `marker`, `nudge`, `cycle`, `drop`, `replace`, `click`, `solo` |
| 0x5B–0x5F | `rewind`, `fast_forward`, `stop`, `play`, `record` |
| 0x60–0x65 | `up`, `down`, `left`, `right`, `zoom`, `scrub` |
| 0x66–0x67 | `user_a`, `user_b` |
| 0x68–0x6F | `fader_touch_1`–`fader_touch_8` |
| 0x70 | `master_touch` |
| 0x71–0x73 | `smpte_led`, `beats_led`, `rude_solo_led` (LEDs) |
| 0x76 | `relay` |

The `fader_touch_*` names can be given to a button; nothing sends them automatically (§3.7).

### 3.2 Messages, device → host

| Component | Message |
|---|---|
| button `mcu: <name>` | Note On ch 1, note per §3.1, `7F` press / `00` release |
| analog `mcu: fader_1`–`fader_8` / `master` | pitch bend ch 1–8 / ch 9, 14 bit (the 12-bit reading shifted left by 2). Sent when the 7-bit value changes, so 128 steps over the travel |
| encoder `mcu: vpot_1`–`vpot_8` | CC 0x10–0x17 ch 1, relative: `01`–`0F` = +1..+15 steps (clockwise), `41`–`4F` = −1..−15; a step is a detent, or more on a quick turn with acceleration (`core/README.md` "Encoder acceleration") |
| (replies) | SysEx, §3.5 |

### 3.3 Faders, host → device

Pitch bend on ch 1–8 (fader 1–8) or ch 9 (master): the top 7 bits become the fader's pick-up target (§2.5). There are no motors.

### 3.4 V-Pot rings

CC 0x30–0x37 ch 1 (V-Pot 1–8), value `0 c mm pppp`:

| Bits | Meaning |
|---|---|
| 6 (`c`) | centre LED: the encoder's `ring_center:` LED on/off |
| 4–5 (`mm`) | mode: 0 dot, 1 boost/cut, 2 wrap, 3 spread |
| 0–3 (`pppp`) | position: 0 = ring off, 1..11 along the ring (12–15 count as 11) |

The device spreads MCU's 11 positions over however many LEDs the encoder's `ring:` lists (1–16, leftmost first). Position 1 is the first LED and 11 the last (spread: 1 is the narrowest, 6 and up every LED).

| Mode | Lit |
|---|---|
| dot | the LED at the position |
| boost/cut | from the middle LED to the position |
| wrap | from the first LED to the position |
| spread | the middle LED and a width either side that grows with the position |

The centre LED follows bit 6 also when the position is 0. With `feedback.script` on (the default), `ring:` and `ring_center:` also work without `mcu:`, from FB_RING (`sysex.md` §4.6.2).

### 3.5 SysEx

`F0 00 00 66 <id> <cmd> … F7`, on the MCU port:

| Host sends | Device does |
|---|---|
| `00` device query | replies `01 <serial, 7 bytes> <challenge 01 02 03 04>` |
| `02 <serial> <response 4>` | replies `03 <serial>`. The response isn't checked, so no response algorithm is involved |
| `12 <pos> <text…>` LCD text | passed to the hook `gm_mcu_on_lcd(pos, text, n)` in pieces of up to 8 characters; text past position 111 (2 rows × 56) is dropped |
| `13 00` version request | replies `14 <5 ASCII>`: `V`, the major version's last digit, `.`, the minor version as 2 digits (firmware 0.2.0 → `V0.02`). Ableton Live asks for this |
| `20 <strip 0..7> <mode>` meter mode | stored (`gm_mcu_meter_mode()`: bit 0 signal LED, bit 1 peak hold, bit 2 LCD meter) |
| `61` | every MCU fader's pick-up target set to 0 |
| `62` | every MCU-driven LED off (button LEDs, `feedback: { mcu }` LEDs, rings and their centre LEDs) |
| `63` reset | `62` and `61`, plus the 12 characters and the meter modes cleared |
| anything else (`0A`–`0E` settings, `0F` go offline, `21` LCD meter mode, …) | ignored |

The serial is the last 7 hex digits of the chip UID, as ASCII. Replies use the same device id (`14` or `15`).

### 3.6 Display characters and meters

| Host sends | Device does |
|---|---|
| CC 0x40–0x4B ch 1: one of the 12 timecode / assignment characters (bit 6 decimal point, bits 0–5 the character) | stored (`gm_mcu_char()`, `gm_mcu_char_ascii()`: codes 0x00–0x1F are `@`–`_`, 0x20–0x3F are space–`?`) and passed to the hook `gm_mcu_on_char` |
| Channel pressure ch 1, data `strip << 4 | level` (level 0..12, 14 overload set, 15 overload clear) | passed to the hook `gm_mcu_on_meter`. A device that shows meters decays them itself (about one step per 300 ms) |

The hooks do nothing by default. No current device has a display or meter component, so none of this is visible yet (#18).

### 3.7 Not implemented

Tracked in #18:
- a display component to show the LCD text and the 12 characters, and meters on LED bars or a display;
- fader touch (notes 0x68–0x70): needs touch-sensing faders, or "touchless" touch from fader movement;
- extender chaining (a second bank of 8 as MCU XT on another cable); `extender: true` only changes the device id;
- the settings messages `0A`–`0E`, and `21` (LCD meter mode).

Not planned: checking the handshake response. No DAW has been tried yet; whether Live, Bitwig, REAPER and Logic accept the unchecked handshake and this subset is part of #18.

## 4. DAW scripts (F3)

A script inside the DAW drives the device by component index over gemifi SysEx on the main port: GET_COMPONENT to learn the controls and their `daw:` roles, EVENT messages for presses, and fire-and-forget FB_* notifications for LEDs, values, rings and text. The protocol is `protocol/sysex.md` §4.6; the scripts and the role names are in `hosts/` (`hosts/README.md`). On with `feedback: { script: true }` (the default; off with `enabled: false`). Follow-ups: #21.

F3 costs more bytes per update than F2: an FB_RING frame is 17 bytes (6 USB-MIDI events), an MCU ring CC 3 (one event). Costs and the rates scripts should use: `protocol/daw-scripts.md` §5.12.

How F3 states fit with the rest of this document: they're host states that don't time out (§2.6), `owner: local` LEDs ignore them, FB_VALUE sets the same pick-up targets and absolute-encoder values as §2.2 and §2.5, and FB_RING draws rings as in §3.4.
