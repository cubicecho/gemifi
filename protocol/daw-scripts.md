# Writing a DAW script for gemifi devices

**The base for every gemifi DAW integration:** the Ableton Live script, the Bitwig extension, a Zynthian driver, and any new one. It describes everything a script needs to find a gemifi device, learn its controls, hear them, and drive its LEDs, knobs and rings, without reading the rest of the gemifi codebase.

- **Status:** protocol v1, host feedback as merged (2026-11-03). Implemented by the firmware, `tools/host`, `web/updater`, [`hosts/ableton-live`](../hosts/ableton-live/), [`hosts/bitwig`](../hosts/bitwig/) and the Zynthian driver ([`hosts/zynthian`](../hosts/zynthian/), code in a zynthian-ui fork). Not yet run on hardware or in a DAW (gemifi #26).
- **Normative source:** [`sysex.md`](sysex.md), mostly §1–§3 and §4.6. This guide restates the parts a script needs, in the order a script uses them. If the two ever disagree, `sysex.md` wins; please report it.
- **Plain MIDI feedback and Mackie Control** (what a device does *without* a script) are in [`host-feedback.md`](host-feedback.md). A script doesn't need them.

Contents:
1. [The model](#1-the-model)
2. [Transport and framing](#2-transport-and-framing)
3. [A script's session](#3-a-scripts-session)
4. [Message reference](#4-message-reference)
5. [Behaviour rules](#5-behaviour-rules)
6. [Roles](#6-roles)
7. [Writing the script: structure and checklist](#7-writing-the-script-structure-and-checklist)
8. [Compatibility and versioning](#8-compatibility-and-versioning)
9. [A complete session, byte by byte](#9-a-complete-session-byte-by-byte)

---

## 1. The model

**Device.** A gemifi device is a MIDI controller (keyboard, fader box, foot controller…) running gemifi firmware built from a device YAML. Over USB it has one MIDI port, the **main port**, named `gemifi (<friendly name>)`. A device with Mackie Control has a second port, `… DAW`, which a script **doesn't use**.

**Main port traffic.** The main port carries two things at once:
- the device's **normal MIDI**: notes, CCs, pitch bend. A script doesn't need to touch it; it keeps reaching tracks and instruments as usual;
- gemifi's **SysEx protocol**: everything in this document.

**Components.** The device describes itself as a list of components, each addressed by **kind** and **index**:

| kind | What | Index order |
|---|---|---|
| 1 | button | YAML order |
| 2 | key matrix (the keyboard; at most one) | 0 |
| 3 | analog input (pot, slider, fader, expression) | YAML order |
| 4 | encoder | YAML order |
| 5 | LED | YAML order; every LED, including a button's own, ring LEDs and centre LEDs |
| 6 | display | (none exist yet) |

Each component has an **id** (the YAML `id:`), a display **name**, and a **role**.

**Roles.** A role is a short string saying what a script should connect the component to: `play`, `volume_3`, `device_param_1`… (§6). It comes from the YAML (`daw: play`), or from the component's Mackie Control name (`mcu: fader_1`). A script maps roles to its DAW's functions and ignores roles it doesn't know. The device never interprets roles; it only passes them on.

**Index, not MIDI.** A script hears and drives components **by index**, never by note or CC:
- presses and moves arrive as **EVENT** messages;
- LEDs, pick-up targets and rings are set with **FB_\*** messages.

So a script works with any gemifi device, and a preset that changes CC numbers doesn't break it.

**Host states.** What a script sets (an LED on, a pick-up target) is a *host state* on the device. It stays until the script changes or releases it; it doesn't time out. The device keeps its own *local* states (an LED lit while its button is held) and shows the host's instead while a host state is set, unless an LED is `owner: local`.

## 2. Transport and framing

### 2.1 Frames
Every message is one SysEx frame on the main port:
```
F0  7D  67 6D  ver  cmd  seq  len_lo len_hi  payload…  chk  F7
```

| Field | Meaning |
|---|---|
| `7D` | Manufacturer ID. `0x7D` is MIDI's non-commercial ID, a **placeholder** until gemifi has its own (gemifi #3). The real one will be 3 bytes (`00 xx yy`). **Keep the ID in one constant and find every later field from its length**, never from fixed offsets. |
| `67 6D` | Signature, ASCII `gm`. A frame without it isn't gemifi's: ignore it. |
| `ver` | Protocol version, `01`. |
| `cmd` | The message number (§2.3). |
| `seq` | 0..127. On a request, chosen by the host and echoed in the reply. |
| `len_lo len_hi` | Payload length in bytes, 14-bit, low 7 bits first. |
| `chk` | `(sum of all bytes from ver to the last payload byte) & 0x7F`. |

Every byte between `F0` and `F7` is 7-bit. A frame whose checksum or length is wrong is dropped, and so is one with an unknown `ver`.

### 2.2 Encodings

| Name | Bytes | Encoding |
|---|---|---|
| byte | 1 | 0..127 |
| u14 | 2 | 7 bits each, least significant first: `v & 0x7F, v >> 7` |
| u28 | 4 | 7 bits each, least significant first |
| u32 | 5 | 7 bits each, least significant first (the last byte holds the top 4 bits) |
| s32 | 5 | the u32 of the two's-complement bit pattern (−1 = `7F 7F 7F 7F 0F`) |
| lstring | 1 + n | `n` (0..31), then `n` bytes of printable ASCII (0x20–0x7E) |
| packed | ⌈8n/7⌉ | 8→7 packing: each group of up to 7 bytes becomes 1 byte of high bits (bit *i* = bit 7 of byte *i*), then the bytes with bit 7 cleared. Only HELLO's `uid` uses it here |

Reference code in Python (MIT; from [`hosts/ableton-live/gemifi/protocol.py`](../hosts/ableton-live/gemifi/protocol.py), which has the rest):
```python
HEADER = bytes([0xF0, 0x7D, 0x67, 0x6D])      # F0 + manufacturer ID + "gm"

def u14(v):  return bytes([v & 0x7F, (v >> 7) & 0x7F])
def dec_u14(b): return b[0] | b[1] << 7

def build_frame(cmd, seq, payload=b""):
    body = bytes([1, cmd, seq]) + u14(len(payload)) + payload
    return HEADER + body + bytes([sum(body) & 0x7F, 0xF7])

def parse_frame(m):                            # -> (cmd, seq, payload) or None
    if len(m) < len(HEADER) + 7 or m[:len(HEADER)] != HEADER or m[-1] != 0xF7:
        return None
    body, chk = m[len(HEADER):-2], m[-2]
    if body[0] != 1 or sum(body) & 0x7F != chk or dec_u14(body[3:5]) != len(body) - 5:
        return None
    return body[1], body[2], bytes(body[5:])
```

### 2.3 Message numbers and the two kinds of traffic

| `cmd` | What |
|---|---|
| `0x00–0x1F` | **requests**, host → device. Each gets exactly one reply |
| `0x40–0x5F` | **replies**: `request | 0x40`, same `seq` |
| `0x20–0x3F` | **notifications**, host → device. **Never answered**, not even with an error |
| `0x60–0x7F` | **unsolicited**, device → host (EVENT, BOOTED, …). Never answered |

**Requests are stop-and-wait:**
- Send one, then wait for its reply (`cmd | 0x40`, same `seq`) for **200 ms**.
- On a timeout, or a reply with status BUSY (9) or BAD_CHECKSUM (1), resend the **same frame with the same `seq`**, up to 5 times (6 sends in all), then give up.
- Use a **new `seq`** for each new request. The device treats a repeated `seq` as a resend and replays its previous reply without redoing the work.
- Only one request is outstanding at a time.

**Notifications are fire-and-forget:**
- Send them any time, including while a request is waiting.
- `seq` is ignored (send 0), and they don't affect resend detection.
- No retries: they're idempotent, so the next update fixes a lost one.

**Reply status.** A reply's payload starts with a status byte. If it isn't OK, nothing follows it.

| Code | Name | When a script sees it |
|---|---|---|
| 0 | OK | |
| 1 | BAD_CHECKSUM | the frame was corrupted: resend |
| 2 | BAD_CMD | the device doesn't know the command (older firmware, or the bootloader) |
| 3 | BAD_LEN | wrong payload length: a script bug |
| 9 | BUSY | resend |
| 10 | BAD_PARAM | GET_COMPONENT past the end, or no component of that kind; unknown param |
| 11 | BAD_VALUE | SET_PARAM value out of range |

(4–8 are firmware-update errors, which a script doesn't see.)

## 3. A script's session

```
script                                   device
  │  HELLO ───────────────────────────────▶ │
  │ ◀─────────────── reply: caps bit 6 set? │  no → stay idle, send nothing else
  │  GET_COMPONENT kind 1 index 0 ────────▶ │
  │ ◀──────────────────── count, id, role … │  … every index of every kind
  │  GET_PARAM debug.events ──────────────▶ │  (remember the old value)
  │  SET_PARAM debug.events = 1 ──────────▶ │
  │ ◀───────────────────────────────── OK   │
  │  FB_* (initial state of everything) ──▶ │  ready
  │                                         │
  │ ◀──────────────────── EVENT (press …)   │  → DAW action
  │  FB_* (only what changed) ────────────▶ │  ← DAW state change
  │  GET_PARAM debug.events (keep-alive) ─▶ │  every ~30 s (~5 s on older firmware)
  │ ◀─────────────────────── BOOTED         │  device restarted → start again at GET_COMPONENT
  │                                         │
  │  FB_RELEASE 0 ────────────────────────▶ │  on exit
  │  SET_PARAM debug.events = old ────────▶ │
```

1. **Find.** Send HELLO on the port the user assigned to the script.
   - No reply after the retries: wait ~3 s and try again (unplugged, or booting).
   - `role` = 1 (bootloader): a firmware update is running; wait and try again.
   - **`caps` bit 6 clear:** not a gemifi device with host feedback (or older firmware). **Stay idle and send nothing in `0x20–0x3F` to it** (§8).
2. **Enumerate.** For each kind 1..6:
   - GET_COMPONENT index 0. BAD_PARAM means none of this kind;
   - otherwise the reply's `count` says how many, so ask for indexes 1..count−1.
   - Keep each component's fields (§4.4).
3. **Enable EVENTs.** GET_PARAM `debug.events` (param id **13**, index 0) and remember the value, then SET_PARAM it to 1.
4. **Ready.** Bind roles to DAW functions (§6), then send the current state of everything (§5).
5. **Run:**
   - **EVENTs** → DAW actions.
   - **DAW changes** → FB_\* for what changed.
   - **Keep-alive:** GET_PARAM `debug.events` periodically; a timeout or a value of 0 means the device restarted or went away, so start again at step 1 (or 2).
   - **BOOTED:** the device restarted and lost every host state; start again at step 2.
6. **Exit.** FB_RELEASE 0 (every LED and value back to the device), then SET_PARAM `debug.events` back to the remembered value.

## 4. Message reference

All examples are real frames from [`test-vectors.json`](test-vectors.json) or computed with the reference codec.

### 4.1 HELLO (0x01, request)
- **Request:** no payload.
- **Reply** (after the status byte):

| Field | Type | Notes |
|---|---|---|
| `role` | byte | 0 app, 1 bootloader |
| `board_id` | u14 | which hardware (`sysex.md` §7) |
| `fw_version` | u32 | `major << 24 | minor << 16 | patch` |
| `proto_version` | byte | 1 |
| `max_chunk` | u14 | firmware upload; ignore |
| `caps` | u14 | bit 1 config, bit 2 LOG, bit 3 EVENT, bit 5 presets, **bit 6 host feedback** (everything in this guide) |
| `uid` | 14 bytes, packed | 12-byte chip ID: tells two identical devices apart |
| `node` | `len` (1..31) + ASCII, optional | the YAML node name, e.g. `keystation-mini32`; absent from the bootloader |

Ignore any bytes after the fields you know.

```
request, seq 5:   F0 7D 67 6D 01 01 05 00 00 07 F7
reply (app, board 1, v0.1.5, caps 0x6E, node "keystation-mini32"):
                  F0 7D 67 6D 01 41 05 2E 00 00 00 01 00 05 00 04 00 00 01 00 00 6E 00 00 10 11 12 13 14 15 16 00 17 18 19 1A 1B 11 6B 65 79 73 74 61 74 69 6F 6E 2D 6D 69 6E 69 33 32 0B F7
```

### 4.2 GET_PARAM (0x03) and SET_PARAM (0x04), requests
- **GET_PARAM:** request `id` u14, `index` u14; reply `id` u14, `index` u14, `value` s32.
- **SET_PARAM:** request `id` u14, `index` u14, `value` s32; reply the same three, with the value now in effect.
- A script only needs param **13, `debug.events`** (0/1). It's **volatile**: never stored, so it's 0 after every reboot.
```
GET_PARAM debug.events, seq 0x20:   F0 7D 67 6D 01 03 20 04 00 0D 00 00 00 35 F7
  reply value 1:                    F0 7D 67 6D 01 43 20 0A 00 00 0D 00 00 00 01 00 00 00 00 7C F7
SET_PARAM debug.events = 1, seq 16: F0 7D 67 6D 01 04 10 09 00 0D 00 00 00 01 00 00 00 00 2C F7
  reply:                            F0 7D 67 6D 01 44 10 0A 00 00 0D 00 00 00 01 00 00 00 00 6D F7
```

### 4.3 GET_COMPONENT (0x11, request)
- **Request:** `kind` (byte), `index` u14. A request that isn't 3 bytes long replies BAD_LEN.
- **Reply:** described in §4.4. An index past the end, a kind with no components, or an unknown kind replies **BAD_PARAM**.
```
request button 0, seq 0x50:  F0 7D 67 6D 01 11 50 03 00 01 00 00 66 F7
reply BAD_PARAM:             F0 7D 67 6D 01 51 55 01 00 0A 32 F7
```

### 4.4 Component description (GET_COMPONENT reply)
Common fields, after the OK status:

| Field | Type | |
|---|---|---|
| `kind` | byte | echoes the request |
| `index` | u14 | echoes the request |
| `count` | u14 | how many components of this kind |
| `id` | lstring | YAML id (cut at 31 characters) |
| `name` | lstring | display name |
| `role` | lstring | §6; empty = no role |

Then, by kind:

| kind | Extra fields, in order |
|---|---|
| 1 button | `led` u14: the index of its LED (kind 5), or 0x3FFF for none |
| 2 key matrix | `keys` u14, `first_note` byte |
| 3 analog | `takeover` byte: 0 none, 1 pick-up |
| 4 encoder | `ring` byte (ring LEDs, 0 = none), `center` u14 (centre LED, 0x3FFF = none), `mode` byte: 0 absolute, 1 relative two's complement, 2 relative offset 64, 3 Mackie Control V-Pot |
| 5 LED | `colours` byte: 1 single, 2 bicolour, 3 RGB (reserved); `owner` byte: 0 host_then_local, 1 host, 2 local |
| 6 display | `cols` byte, `rows` byte, `type` byte: 1 characters, 2 segments |

Notes:
- **Fields added later.** `takeover`, `mode` and `owner` were added after the first release, and older firmware doesn't send them. **Treat a missing field as "not reported"** and assume FB_VALUE / FB_LED apply. **Ignore bytes after the fields you know**, since more may follow.
- **No component has index 0x3FFF**, so 0x3FFF always means "none".
- **One LED index space.** A button's `led` and an encoder's `center` index into the kind-5 list, and so do the encoder's ring LEDs. They're ordinary LEDs, so FB_LED can set them too.

```
button 0 of 2, id "play", name "Play", role "play", LED 3:
  F0 7D 67 6D 01 51 50 17 00 00 01 00 00 02 00 04 70 6C 61 79 04 50 6C 61 79 04 70 6C 61 79 03 00 4D F7
encoder 0 of 8, id "enc1", name "Encoder 1", role "device_param_1", ring 11, centre LED 200, mode 0:
  F0 7D 67 6D 01 51 54 28 00 00 04 00 00 08 00 04 65 6E 63 31 09 45 6E 63 6F 64 65 72 20 31 0E 64 65 76 69 63 65 5F 70 61 72 61 6D 5F 31 0B 48 01 00 31 F7
the same from older firmware (no mode byte):
  F0 7D 67 6D 01 51 5E 27 00 00 04 00 00 08 00 04 65 6E 63 31 09 45 6E 63 6F 64 65 72 20 31 0E 64 65 76 69 63 65 5F 70 61 72 61 6D 5F 31 0B 48 01 3A F7
```

### 4.5 EVENT (0x61, unsolicited)
- **Payload:** `type` byte, `index` u14, `value` u14, `t_ms` u28 (device time, ms).
- **Sent only while `debug.events` = 1.**

| type | Component | `value` |
|---|---|---|
| 1 | button | 1 down, 0 up |
| 2 | key | velocity; 0 = release (keys also send their normal notes, so a script usually ignores these) |
| 3 | analog | the calibrated position, 0..16383. With `takeover: pickup`, held back until the knob picks up the device's target (§5.3) |
| 4 | encoder | the change in **detents** since the last EVENT, 14-bit two's complement (≥ 8192 means negative: subtract 16384) |

Other types are reserved; ignore them.
```
button 2 down:     F0 7D 67 6D 01 61 07 09 00 01 02 00 01 00 52 09 00 00 51 F7
encoder 0, −1:     F0 7D 67 6D 01 61 08 09 00 04 00 00 7F 7F 14 0A 00 00 13 F7
analog 1 at 8192:  F0 7D 67 6D 01 61 09 09 00 03 01 00 00 40 78 0A 00 00 3A F7
```

### 4.6 BOOTED (0x64, unsolicited)
- **Payload:** the HELLO reply's fields without the status byte (§4.1).
- **When:** sent once, about 1.5 s after the device starts, only by firmware with caps bit 6.
- **What it means:** every host state is gone and `debug.events` is 0. Start again at enumeration (§3, step 2).
- **It can be missed** if the port opens later than that, which is why §3 keeps a keep-alive.
```
F0 7D 67 6D 01 64 09 29 00 00 10 7E 00 00 08 08 00 01 00 00 6E 00 00 10 11 12 13 14 15 16 00 17 18 19 1A 1B 0D 66 65 65 64 62 61 63 6B 2D 74 65 73 74 45 F7
```

### 4.7 Notifications (host → device, never answered)

| cmd | Name | Payload |
|---|---|---|
| 0x20 | FB_LED | `led` u14, `level` byte (§5.1) |
| 0x21 | FB_LED_RGB | `led` u14, `r` `g` `b` (bytes, 0..127), `flags` (bit 0 blink) |
| 0x22 | FB_LEDS | `first` u14, `n` byte (1..64), then `n` × `level`: LEDs `first` .. `first + n − 1` |
| 0x23 | FB_VALUE | `kind` byte (3 analog, 4 encoder), `index` u14, `value` u14 (0..16383) |
| 0x24 | FB_RING | `encoder` u14, `mode` byte (0 dot, 1 boost/cut, 2 wrap, 3 spread), `value` u14 (0..16383), `flags` (bit 0 centre LED on, bit 1 ring off) |
| 0x25 | FB_TEXT | `display` u14, `row` byte, `col` byte, then text (printable ASCII, ≤ 64 bytes, no length byte) |
| 0x26 | FB_RELEASE | `what` byte: 0 everything, 1 LEDs, 2 values, 3 text |

- **Unused `flags` bits** are sent as 0; the device ignores them.
- **Drops.** The device drops the whole message for an unknown `kind`, ring `mode` or release `what`, and silently drops a malformed one.
- **Out of range.** An index past the last component is ignored. For FB_LEDS, only the LEDs that exist are set.
```
FB_LED led 3 on:                           F0 7D 67 6D 01 20 00 03 00 03 00 7F 26 F7
FB_LED_RGB led 4 green, blinking:          F0 7D 67 6D 01 21 00 06 00 04 00 00 64 00 01 11 F7
FB_LEDS from 8: off, blink, on, colour 2:  F0 7D 67 6D 01 22 00 07 00 08 00 04 00 01 02 7F 38 F7
FB_VALUE analog 2 = 16383:                 F0 7D 67 6D 01 23 00 05 00 03 02 00 7F 7F 2C F7
FB_RING encoder 1 boost/cut 4096, centre:  F0 7D 67 6D 01 24 00 06 00 01 00 01 00 20 01 4E F7
FB_RING encoder 0 off:                     F0 7D 67 6D 01 24 00 06 00 00 00 02 00 00 02 2F F7
FB_TEXT display 0 row 1 col 4 "Track 1":   F0 7D 67 6D 01 25 00 0B 00 00 00 01 04 54 72 61 63 6B 20 31 7C F7
FB_RELEASE everything:                     F0 7D 67 6D 01 26 00 01 00 00 28 F7
```

## 5. Behaviour rules

### 5.1 LEDs
- **`level`:** 0 off, 1 blink, 2..127 on. It's on/off, not brightness.
- **Bicolour LEDs** (`colours` 2): 2..63 = colour 1, 64..127 = colour 2, 1 = blink colour 1.
- **FB_LED_RGB:** on a single-colour LED, any non-zero `r`/`g`/`b` lights it. On a bicolour LED, colour 1 if `r` ≥ `g`, else colour 2; all zero is off.
- **Persistence:** a script's LED states **don't time out**.
- **`owner` 2 (local):** the LED ignores FB_\*, so don't bind it.
- **`owner` 0 (host_then_local):** the LED shows the script's state while one is set, and its local state after FB_RELEASE.
- **Bank switches:** use FB_LEDS for runs of consecutive LEDs, FB_LED otherwise.

### 5.2 Values (FB_VALUE)
- **Analog with `takeover` 1:** `value` becomes its **pick-up target**. The device keeps the knob silent (no CC, no EVENT) until it reaches or crosses the target, so the DAW parameter doesn't jump when the knob is touched. Send it whenever the mapped parameter changes in the DAW.
- **Analog with `takeover` 0:** ignored; don't send.
- **Encoder with `mode` 0 (absolute):** `value` becomes the encoder's value, so its next turn continues from there.
- **Other encoder modes:** ignored; don't send.
- **Resolution:** values are 14-bit. A device that sends 7-bit CCs uses the top 7 bits.

### 5.3 Pick-up and EVENTs
- EVENTs from a pick-up analog are held back until it picks up, so a script can apply EVENT values directly.
- For analogs **without** device pick-up (`takeover` 0), a script may do its own: ignore EVENT values until they cross the parameter's current value. The Live script and the Bitwig extension do.

### 5.4 Rings (FB_RING)
- `value` 0 is the ring's first LED and 16383 its last.
- **Modes:**
  - dot (0): one LED;
  - boost/cut (1): fills from the middle LED to the position, so 8192 = centre;
  - wrap (2): fills from the first LED;
  - spread (3): fills both ways from the middle, `value` = width.
- **Flags:** bit 0 lights the centre LED (if the encoder has one); bit 1 turns the whole ring off whatever `mode`/`value` say. Send bit 1 when nothing is mapped.
- **Suggested use:** boost/cut for bipolar parameters (pan), wrap for the rest.

### 5.5 Text (FB_TEXT)
- Writes from (`row`, `col`), clipped at the end of the row; it doesn't wrap.
- A position outside the display drops the message.
- The rest of the row is unchanged, so clear old text with spaces.
- Each character takes a place (a `.` too). The device's own text (MIDI values, preset names) stays underneath, and FB_RELEASE 0 or 3 shows it again.
- Displays so far: the Code 61's 6-character 14-segment display (GET_COMPONENT kind 6: 6 cols, 1 row, type characters).

### 5.6 Release (FB_RELEASE)
- 1 (LEDs): every LED state the script set goes back to local, rings included.
- 2 (values): every pick-up target is dropped; encoder values stay.
- 3 (text): every display shows the device's own text again.
- 0: all of them. **Send it on exit.**

### 5.7 Change-only output
Keep a cache of what you last sent for each LED, value and ring, and send only differences. The device can take notifications as fast as USB carries them, but DAW state (meters especially) changes far more often than a controller needs. Don't map meters to LEDs.

## 6. Roles

The vocabulary lives in [`hosts/README.md`](../hosts/README.md); the builder accepts these names, plus `x_<anything>` for a script's own use. A script **must ignore** roles it doesn't know or doesn't support, logging each one once.

| Group | Roles | On a button / LED | On an analog / encoder |
|---|---|---|---|
| Transport | `play`, `stop`, `record`, `loop`, `metronome`, `overdub`, `session_record`, `automation_arm`, `tap_tempo`, `undo`, `redo`, `rewind`, `fast_forward` | press = the action; the LED shows the state (e.g. `record` lit while recording, blinking while armed but stopped). Momentary actions (`stop`, `undo`, `tap_tempo`…) light only while held; `rewind` / `fast_forward` repeat while held | |
| Selected track | `track_prev`, `track_next`, `track_mute`, `track_solo`, `track_arm` | the action / its state | |
| | `track_volume`, `track_pan`, `send_a`, `send_b` | | the parameter |
| Mixer bank of 8 | `mute_N`, `solo_N`, `arm_N`, `select_N` (N = 1..8), `bank_prev`, `bank_next` | strip N of the current bank; banking moves by 8 and refreshes everything | |
| | `volume_N`, `pan_N` | | strip N's parameter |
| Master | `master_volume`, `cue_volume`, `crossfader` | | the parameter |
| | `tempo` | | encoder: tempo ± |
| Selected device | `device_param_N` (N = 1..8) | | the device's Nth parameter of the current page (with a ring: FB_RING) |
| | `device_prev`, `device_next`, `device_bank_prev`, `device_bank_next`, `device_on` | navigation / on-off state | |
| Session | `scene_launch`, `scene_prev`, `scene_next`, `stop_all_clips` | the action | |

**Mackie Control names used as roles** (when a component has `mcu:` but no `daw:`) map like this:
- `fader_N` → `volume_N`;
- `master` → `master_volume`;
- `vpot_N` → `pan_N`;
- `rec_N` → `arm_N`;
- `solo_N`, `mute_N`, `select_N` stay as they are;
- `bank_left` / `bank_right` → `bank_prev` / `bank_next`;
- `channel_left` / `channel_right` → `track_prev` / `track_next`;
- `cycle` → `loop`, `click` → `metronome`;
- `play`, `stop`, `record`, `rewind`, `fast_forward`, `undo` stay as they are.

Lower-case roles before matching.

**Button layers.** A device may give a physical button a second meaning while one of its modes is active (core/README.md "Button layers"): a *twin*, listed as its own button with its own role and its own LED (for example `b2` with `mute_1` and `b2_arm` with `arm_1`, on the same switch and the same LED pins). Treat it like any other button. Only the live one sends EVENTs; set both LEDs anyway: the device keeps each state and shows the live one.

**Which component drives an LED.** A button with a role and an LED shows that role's state on its LED. An LED component (kind 5) with its own role shows that role's state. If both apply to the same LED, the button's role wins.

## 7. Writing the script: structure and checklist

### 7.1 Structure
Every gemifi script so far has split into four layers. The first three need no DAW, so they can be unit-tested:

| Layer | Job | Live (Python) | Bitwig (Java) |
|---|---|---|---|
| protocol | frames, encodings, message builders/parsers | `gemifi/protocol.py` | `core/Protocol.java` |
| device | the session of §3: find, enumerate, enable, keep-alive, BOOTED, retries on a timer tick | `gemifi/device.py` | `core/Device.java` |
| mapping | roles → actions, DAW state → change-only FB_\*, pick-up, banks | `gemifi/mapping.py` | `core/Mapping.java` |
| adapter | the DAW's API: MIDI in/out, a ~100 ms timer, the transport/track/device model | `gemifi/live_song.py`, `surface.py` | `BitwigModel.java`, `GemifiExtension.java` |

Port the first three from either implementation (MIT), then write the adapter for the new DAW. Host-specific notes go in `hosts/<host>/`, e.g. [`hosts/zynthian/`](../hosts/zynthian/).

### 7.2 What the DAW side must provide
- **MIDI:** send and receive **SysEx on the device's main port**, while the device's normal notes/CCs still reach tracks. Check that SysEx reaches the script with no extra mapping; that's one of the open questions in the Live script.
- **A timer** of about 100 ms, for request timeouts, retries and the keep-alive.
- **Observers** for the state the roles show (transport, mixer, selection, device parameters), or polling from the timer.
- **An exit hook** to send FB_RELEASE while the port is still open.

### 7.3 Checklist
- [ ] Manufacturer ID in one constant; fields found from its length (§2.1).
- [ ] Requests: one at a time, 200 ms timeout, same-`seq` resends ×5, a new `seq` per request (§2.3).
- [ ] Notifications: never wait for a reply; seq 0 is fine.
- [ ] **Nothing in 0x20–0x3F unless caps bit 6 is set** (§3, §8).
- [ ] Bootloader (`role` 1) and no-reply cases handled (idle, retry).
- [ ] Enumeration walks every kind from index 0 using `count`; BAD_PARAM = none.
- [ ] Unknown trailing bytes ignored; missing added fields = "not reported" (§4.4).
- [ ] `debug.events` remembered, set to 1, restored on exit.
- [ ] BOOTED → re-enumerate and re-send; keep-alive as a fallback (~30 s with BOOTED/added fields seen, ~5 s otherwise).
- [ ] No FB_VALUE to `takeover` 0 analogs or relative encoders; no FB_LED to `owner` 2 LEDs (§5).
- [ ] Change-only output; FB_LEDS for runs.
- [ ] Unknown and unsupported roles ignored (logged once); `mcu:` names mapped (§6).
- [ ] FB_RELEASE 0 on exit.
- [ ] The codec tested against [`test-vectors.json`](test-vectors.json): the `frames` and `bad_frames` sections, and the `feedback` section (every GET_COMPONENT reply, with and without the added fields, every FB_\*, and BOOTED).
- [ ] Several devices at once: one session per port, no shared state.

### 7.4 Testing without hardware
- **Fakes:** a fake device that answers HELLO, GET_COMPONENT, GET_PARAM and SET_PARAM per §4, records FB_\*, and can send EVENT and BOOTED (and drop a reply, to test resends), plus a fake DAW model. Both existing scripts have one to copy: [`hosts/ableton-live/tests/fakes.py`](../hosts/ableton-live/tests/fakes.py) and `hosts/bitwig/src/test/java/gemifi/bitwig/core/FakeDevice.java`.
- **`gemifi.py`** from [`tools/host`](../tools/host/) against a real device:
  - `gemifi.py components` lists what the device reports;
  - `gemifi.py fb led <id> on` drives it by hand;
  - `gemifi.py log` shows the traffic.
- **CI:** a new script in this repo should get a CI job like the Bitwig one, so a protocol change can't break it unnoticed.

## 8. Compatibility and versioning

- **Older firmware.** A device without caps bit 6 answers an unknown command with BAD_CMD as `cmd | 0x40`. For `0x20–0x3F` that lands in the unsolicited range (`0x60–0x7F`) and would read as a LOG or EVENT. **Never send notifications to it.**
- **Additions are append-only.** New fields go at the end of a reply or message, so ignore bytes you don't know and treat missing ones as "not reported".
- **New components, roles and messages:**
  - new kinds answer BAD_PARAM on older firmware;
  - new roles are strings a script ignores until it supports them;
  - new unsolicited messages (`0x60–0x7F`) should be ignored.
- **`proto_version` 1** covers everything here. A change that isn't backward compatible would bump it; a script should then stay idle on a version it doesn't know.
- **The manufacturer ID will change** from `0x7D` to a 3-byte ID (#3) before public releases. With the ID in one constant and offsets derived from it (§2.1), that's a one-line change.

## 9. A complete session, step by step

A device with one button (`play`, LED 0) and one fader (`volume_1`, pick-up). Frames for each message type are in §4.

```
→ HELLO
← HELLO reply: app, caps 0x6E (bit 6 set)
→ GET_COMPONENT kind 1 index 0   ← button 0 of 1, "play", role "play", led 0
→ GET_COMPONENT kind 2 index 0   ← BAD_PARAM (no key matrix)
→ GET_COMPONENT kind 3 index 0   ← analog 0 of 1, "fader", role "volume_1", takeover 1
→ GET_COMPONENT kind 4 index 0   ← BAD_PARAM
→ GET_COMPONENT kind 5 index 0   ← LED 0 of 1, colours 1, owner 0
→ GET_COMPONENT kind 6 index 0   ← BAD_PARAM
→ GET_PARAM 13                   ← value 0 (remember it)
→ SET_PARAM 13 = 1               ← OK                      ready
→ FB_LED 0 = 0                   (the transport is stopped)
→ FB_VALUE analog 0 = 11000      (track 1's volume, as the pick-up target)

  the user presses Play
← EVENT button 0 = 1             → the DAW starts playing
→ FB_LED 0 = 127

  the user moves the fader up past 11000 (silent until then)
← EVENT analog 0 = 12000         → track 1's volume
  the volume is changed with the mouse
→ FB_VALUE analog 0 = 9000       (the fader is silent again until it reaches 9000)

  the device is power-cycled
← BOOTED                         → start again at GET_COMPONENT kind 1

  the DAW closes
→ FB_RELEASE 0
→ SET_PARAM 13 = 0
```
