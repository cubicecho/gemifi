# Writing a DAW script for gemifi devices

**The base for every gemifi DAW integration:** the Ableton Live script, the Bitwig extension, a Zynthian driver, and any new one. It describes everything a script needs to find a gemifi device, learn its controls, hear them, and drive its LEDs, knobs and rings, without reading the rest of the gemifi codebase.

- **Status:** protocol v1, host feedback as merged (2026-11-03), plus pads, pad and encoder twins, touch, display roles and the new roles (2026-11-04, #21), and cycle layers, the `layer` byte and EVENT 8 (2026-11-04, encoder modes). Meters and motor faders are reserved: no firmware has them yet. Implemented by the firmware, `tools/host`, `web/updater`, [`hosts/ableton-live`](../hosts/ableton-live/), [`hosts/bitwig`](../hosts/bitwig/) and the Zynthian driver ([`hosts/zynthian`](../hosts/zynthian/), code in a zynthian-ui fork). Not yet run on hardware or in a DAW (gemifi #26).
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
| 6 | display | YAML order |
| 7 | pad | YAML order, then the pad twins (§6, "Layers") |
| 8 | meter | reserved; no firmware has meters yet |

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
  │ ◀────────── EVENT 8 for each layer on   │  the replay, before or after the OK
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
2. **Enumerate.** For each kind 1..8 the script uses:
   - GET_COMPONENT index 0. BAD_PARAM means none of this kind. Firmware from before kinds 7 and 8 answers BAD_PARAM for them, so that's "none" too;
   - otherwise the reply's `count` says how many, so ask for indexes 1..count−1.
   - Keep each component's fields (§4.4).
   - Send each request as soon as the previous reply arrives: a big device has a couple of hundred components (§5.12).
3. **Enable EVENTs.** GET_PARAM `debug.events` (param id **13**, index 0) and remember the value, then SET_PARAM it to 1.
   - Setting it to 1 makes the device **replay its layers**: one EVENT 8 with value 1 for each mode that is on (§4.5). That tells the script which twins are live (§5.11).
   - The replay can arrive before the SET_PARAM reply. Keep EVENT 8 from the moment you send the SET_PARAM, not only once ready.
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
reply BAD_PARAM:             F0 7D 67 6D 01 51 50 01 00 0A 2D F7
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
| 1 button | `led` u14: the index of its LED (kind 5), or 0x3FFF for none; `layer` byte |
| 2 key matrix | `keys` u14, `first_note` byte |
| 3 analog | `takeover` byte: 0 none, 1 pick-up, 2 motor (reserved); `touch` byte: 0 none, 1 sensor, 2 from movement |
| 4 encoder | `ring` byte (ring LEDs, 0 = none), `center` u14 (centre LED, 0x3FFF = none), `mode` byte: 0 absolute, 1 relative two's complement, 2 relative offset 64, 3 Mackie Control V-Pot; `layer` byte |
| 5 LED | `colours` byte: 1 single, 2 bicolour, 3 RGB (reserved); `owner` byte: 0 host_then_local, 1 host, 2 local; `layer` byte |
| 6 display | `cols` byte, `rows` byte, `type` byte: 1 characters, 2 segments |
| 7 pad | `led` u14: the index of its LED (kind 5), or 0x3FFF for none, as for a button; `layer` byte |
| 8 meter (reserved) | `segments` byte; `type` byte: 1 LED bar, 2 encoder ring, 3 display row |

Notes:
- **Fields added later.** `takeover`, `mode` and `owner` were added after the first release, `touch` after them, and `layer` last (on buttons after `led`, encoders after `mode`, LEDs after `owner`, pads after `led`). Older firmware doesn't send them. **Treat a missing field as "not reported"**: assume FB_VALUE / FB_LED apply, and a missing `layer` means 0. **Ignore bytes after the fields you know**, since more may follow.
- **`layer`:** 0 = always there (not a twin). n = a twin in mode n − 1, live only while that mode is on (§5.11). Firmware from before the byte already had twins; it doesn't say which they are, so a script treats everything as live.
- **No component has index 0x3FFF**, so 0x3FFF always means "none".
- **One LED index space.** A button's or pad's `led` and an encoder's `center` index into the kind-5 list, and so do the encoder's ring LEDs. They're ordinary LEDs, so FB_LED can set them too.
- **Kinds 7 and 8 are newer.** Firmware from before them answers BAD_PARAM: treat that as "none of that kind". Current firmware also answers BAD_PARAM for kind 8 (no meters yet).
- **Pads (kind 7).** `count` covers the pads and then the pad twins (§6, "Layers"). `role` is the YAML `daw:`. Base pads play notes and send no EVENTs, so only a pad twin's role does anything (§5.9).
- **`takeover` 2 = motor** (reserved): FB_VALUE moves the fader (§5.2). No firmware has it yet.
- **`touch`:** 1 = a touch sensor (reserved), 2 = "touched" worked out from movement. Either way the analog sends EVENT type 5 (§4.5). 0 = no touch EVENTs.
- **Displays (kind 6).** `role` is the display's YAML `daw:` (§5.10). Older firmware always sent it empty.

```
button 0 of 2, id "play", name "Play", role "play", LED 3:
  F0 7D 67 6D 01 51 50 17 00 00 01 00 00 02 00 04 70 6C 61 79 04 50 6C 61 79 04 70 6C 61 79 03 00 4D F7
encoder 0 of 8, id "enc1", name "Encoder 1", role "device_param_1", ring 11, centre LED 200, mode 0:
  F0 7D 67 6D 01 51 54 28 00 00 04 00 00 08 00 04 65 6E 63 31 09 45 6E 63 6F 64 65 72 20 31 0E 64 65 76 69 63 65 5F 70 61 72 61 6D 5F 31 0B 48 01 00 31 F7
the same from older firmware (no mode byte):
  F0 7D 67 6D 01 51 5E 27 00 00 04 00 00 08 00 04 65 6E 63 31 09 45 6E 63 6F 64 65 72 20 31 0E 64 65 76 69 63 65 5F 70 61 72 61 6D 5F 31 0B 48 01 3A F7
analog 0 of 9, id "fader_1", name "Fader 1", role "volume_1", takeover 1, touch 2 (from movement):
  F0 7D 67 6D 01 51 53 21 00 00 03 00 00 09 00 07 66 61 64 65 72 5F 31 07 46 61 64 65 72 20 31 08 76 6F 6C 75 6D 65 5F 31 01 02 58 F7
request pad 28, seq 0x51:  F0 7D 67 6D 01 11 51 03 00 07 1C 00 09 F7
reply: pad 28 of 32 (16 pads, then 16 twins), id "pad_13_pads", name "Pad 13", role "clip_1_1", LED 52:
  F0 7D 67 6D 01 51 51 24 00 00 07 1C 00 20 00 0B 70 61 64 5F 31 33 5F 70 61 64 73 06 50 61 64 20 31 33 08 63 6C 69 70 5F 31 5F 31 34 00 37 F7
request pad 0, seq 0x52, to firmware from before pads:  F0 7D 67 6D 01 11 52 03 00 07 00 00 6E F7
  reply BAD_PARAM (no pads):  F0 7D 67 6D 01 51 52 01 00 0A 2F F7
button 9 of 12, id "b2_arm", name "B2", role "arm_1", LED 20, layer 1 (a twin in mode 0):
  F0 7D 67 6D 01 51 55 19 00 00 01 09 00 0C 00 06 62 32 5F 61 72 6D 02 42 32 05 61 72 6D 5F 31 14 00 01 6F F7
button 1 of 12, id "b2", name "B2", role "mute_1", LED 13, layer 0 (always there):
  F0 7D 67 6D 01 51 56 16 00 00 01 01 00 0C 00 02 62 32 02 42 32 06 6D 75 74 65 5F 31 0D 00 00 36 F7
encoder 12 of 24, id "enc5_zyn", name "Encoder 5", role "device_param_1", no ring, no centre, mode 1, layer 3 (a twin in mode 2):
  F0 7D 67 6D 01 51 57 2D 00 00 04 0C 00 18 00 08 65 6E 63 35 5F 7A 79 6E 09 45 6E 63 6F 64 65 72 20 35 0E 64 65 76 69 63 65 5F 70 61 72 61 6D 5F 31 00 7F 7F 01 03 4F F7
LED 4 of 30, id "led_4", no role, colours 2, owner 0, layer 0:
  F0 7D 67 6D 01 51 58 11 00 00 05 04 00 1E 00 05 6C 65 64 5F 34 00 00 02 00 00 31 F7
```

### 4.5 EVENT (0x61, unsolicited)
- **Payload:** `type` byte, `index` u14, `value` u14, `t_ms` u28 (device time, ms).
- **Sent only while `debug.events` = 1.**

| type | Component | `value` |
|---|---|---|
| 1 | button | 1 down, 0 up |
| 2 | key | velocity; 0 = release (keys also send their normal notes, so a script usually ignores these) |
| 3 | analog | the calibrated position, 0..16383. With `takeover: pickup`, held back until the knob picks up the device's target (§5.3) |
| 4 | encoder | the change in **steps** since the last EVENT, 14-bit two's complement (≥ 8192 means negative: subtract 16384): one per detent, or more on a quick turn when the encoder has acceleration (`core/README.md` "Encoder acceleration"). Use it as the change; the device has already scaled it |
| 5 | touch | `index` is the analog's index. 1 touched, 0 released. Only from analogs with `touch` ≠ 0 (§4.4) |
| 7 | pad | `index` is the pad's index (kind 7, twins included). The peak reading × 4 on a hit (1..16380), 0 on release. **Only pad twins send it** |
| 8 | layer | `index` is a mode index: the n − 1 of a twin's `layer` n (§4.4). 1 the mode turned on, 0 off |

Other types are reserved; ignore them.

- **Touch from movement** (`touch` 2): "touched" comes just before the first analog EVENT of a movement, and "released" once the reading has been still for 300 ms.
- **Pads:** a base pad (not a twin) only plays its note and sends no EVENT. A pad twin, while its layer is active, sends EVENT 7 and plays no note. Treat value > 0 as a press, 0 as the release.
- **Layers (type 8):**
  - sent when a mode turns on or off: a toggle, a hold, a cycle step (§6, "Layers"). A cycle step change sends 0 for the old step, then 1 for the new one;
  - **the replay:** when the host sets `debug.events` to 1, the device sends value 1 once for each mode that is on (§3, step 3);
  - a mode may have no twins (the Code 61's MIDI step): its EVENT 8 matches no `layer`, so nothing changes. Ignore it.

```
button 2 down:     F0 7D 67 6D 01 61 07 09 00 01 02 00 01 00 52 09 00 00 51 F7
encoder 0, −1:     F0 7D 67 6D 01 61 08 09 00 04 00 00 7F 7F 14 0A 00 00 13 F7
analog 1 at 8192:  F0 7D 67 6D 01 61 09 09 00 03 01 00 00 40 78 0A 00 00 3A F7
analog 2 touched:  F0 7D 67 6D 01 61 0A 09 00 05 02 00 01 00 08 27 00 00 2C F7
analog 2 released: F0 7D 67 6D 01 61 0B 09 00 05 02 00 00 00 2C 2A 00 00 53 F7
pad 28 hit, 9000:  F0 7D 67 6D 01 61 0C 09 00 07 1C 00 28 46 70 2E 00 00 26 F7
pad 28 released:   F0 7D 67 6D 01 61 0D 09 00 07 1C 00 00 00 40 2F 00 00 0A F7
mode 2 on:         F0 7D 67 6D 01 61 0E 09 00 08 02 00 01 00 70 2E 00 00 22 F7
mode 1 off:        F0 7D 67 6D 01 61 0F 09 00 08 01 00 00 00 70 2E 00 00 21 F7
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
| 0x27 | FB_METER | reserved: `meter` u14 (kind 8 index), `level` byte (0..127), `flags` (bit 0 clip) (§5.8) |

- **FB_METER is reserved.** Current firmware drops it, as it drops any unknown notification.
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
FB_METER meter 0 at 100:                   F0 7D 67 6D 01 27 00 04 00 00 00 64 00 10 F7
FB_METER meter 8 at 127, clip:             F0 7D 67 6D 01 27 00 04 00 08 00 7F 01 34 F7
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
- **Analog with `takeover` 2 (motor, reserved):** `value` moves the fader. Send it whenever the mapped parameter changes. No firmware has it yet.
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
Keep a cache of what you last sent for each LED, value and ring, and send only differences. The device can take notifications as fast as USB carries them, but DAW state (meters especially) changes far more often than a controller needs. Costs, rates and batching: §5.12. Drive meters only with FB_METER, to meter components (§5.8). Don't build meters out of FB_LED or FB_LEDS.

### 5.8 Meters (FB_METER, reserved)
- Only for components of kind 8, with role `meter_N` or `master_meter`. No firmware has them yet.
- `level` 0..127 is the meter's full range; the device spreads it over its `segments`. `flags` bit 0 lights its clip indicator.
- **Send rises only.** The device draws the fall-off itself. Send a level when the DAW's meter rises; don't send it falling.
- **At most ~25 a second** for each meter (one every 40 ms). Within that time, send the highest reading.

### 5.9 Pads and the clip grid
- **Base pads play notes; twins talk to the script.** A base pad sends no EVENT, so a script can't see it. A pad twin (§6, "Layers") sends EVENT 7 and plays nothing while its layer is active. Its role works as on a button: value > 0 is a press, 0 the release, and its `led` shows the state.
- **Grid.** `clip_T_S` is the clip in column T (track) and row S (scene), both 1..8, counted from the grid's scroll position. The script keeps a column and a row offset, both starting at 0.
  - `grid_left` / `grid_right` move the column offset by one, `grid_up` / `grid_down` the row offset; clamp them to the DAW's tracks and scenes, then refresh every clip LED.
  - `scene_N` launches the scene in row N of the grid.
  - A press toggles the clip: launch it, or stop it while it plays. What a press on an empty slot does is up to the script (nothing, or record).
- **Colours: FB_LED_RGB.** Off for an empty slot. Otherwise the clip's own colour if the DAW has one, or one colour for "has a clip, stopped" and another for "playing" (the Zynthian driver: teal 0,127,127 and blue 0,0,127). Blink (flags bit 0) while a launch or stop is queued. On a single-colour or bicolour LED, FB_LED_RGB still works (§5.1).
- **Set the twin LEDs at any time.** The device keeps them and shows them while the layer is active; the rest of the time the pad LEDs show the device's own state.

### 5.10 Displays
A display (kind 6) with a role shows what the role says. Write it with FB_TEXT (§5.5), fitted to `cols` × `rows`: cut the text, and pad it with spaces.

| Role | Shows |
|---|---|
| `time` | the song position: bars.beats, or timecode after `time_format`. Use as many fields as fit the cols |
| `track_name` | the selected track's name |
| `param` | the name and value of the control being moved; when nothing moves, the selected track (or chain) |
| `status` | short messages from the script: connected, the bank's first strip after a bank move, a mode change |

A script may leave a display without a role alone: it keeps the device's own text.

### 5.11 Layers
- **Live components.** A component with `layer` 0 (or no `layer` byte) is always live. One with `layer` n > 0 is a twin, live only while mode n − 1 is on (§6, "Layers").
- **Tracking it.** Start with no mode on. The replay after SET_PARAM `debug.events` = 1 (§3, step 3) fills in the modes that are on; each EVENT 8 after that turns one on or off. Start again empty after BOOTED or a new enumeration.
- **A script may ignore layers entirely.** The device sends EVENTs only from live components, so binding every component by its role already works. Feedback to a twin that isn't live is harmless: the device keeps its LED state and value and shows them when it is.
- **One role on several components.** Two twins in different modes may have the same role (`device_param_1` on a twin in mode 2 and another in mode 3), and so may a base and a twin. Bind each one: both drive the role, and both get its LED, FB_VALUE and ring state.
- **Using it.** A script may page by what is live. E.g. `device_bank_prev` / `device_bank_next` step by the number of live `device_param_N`: on the Code 61, 4 in its ZYN encoder mode and 8 in DEVICE (§6). The Zynthian driver pages this way, and so does the Live script on firmware that sends `layer` (pages of 8 otherwise); the Bitwig extension keeps Bitwig's remote-control pages of 8.
- **A limit.** The `layer` byte doesn't say which physical control a twin takes over. A base component counts as live even while a twin has its control.

### 5.12 Rates and batching
**What each message costs.** Every frame has 11 bytes of framing (`F0`, the ID, `gm`, `ver`, `cmd`, `seq`, the length, `chk`, `F7`); the 3-byte manufacturer ID will add 2 (§8). USB-MIDI carries SysEx 3 bytes per 4-byte event, 16 events per 64-byte USB packet.

| Message | Bytes | USB-MIDI events |
|---|---|---|
| FB_LED | 14 | 5 |
| FB_LED_RGB | 17 | 6 |
| FB_LEDS, n LEDs | 14 + n | ⌈(14 + n) / 3⌉: 8 LEDs = 22 bytes, 8 events |
| FB_VALUE | 16 | 6 |
| FB_RING | 17 | 6 |
| FB_TEXT, c characters | 15 + c | ⌈(15 + c) / 3⌉: 16 characters = 31 bytes, 11 events |
| FB_RELEASE | 12 | 4 |
| EVENT (device → host) | 20 | 7 |
| GET_PARAM / SET_PARAM request | 15 / 20 | 5 / 7 |
| GET_COMPONENT request / reply | 14 / about 30–80 | 5 / 10–27 |

For comparison, a Mackie Control ring or LED update is one 3-byte message (one event).

**A bank change** (8 FB_RING, 8 FB_VALUE, 8 LEDs in one FB_LEDS, 2 FB_TEXT rows of 16 characters) is 348 bytes, 126 events, 8 USB packets; with 8 separate FB_LED instead of the FB_LEDS, 438 bytes. On USB that is well under a millisecond of bus time. On a serial link it is 30 ms at 115200 baud (the ESP serial transport's default) and 111 ms at 31250. The Code 61 and the Keystation speak this protocol over USB only (the Code 61's DIN port carries plain MIDI).

**How the device takes it.**
- **USB:** nothing is lost to a burst. The device reads one USB packet (up to 48 SysEx bytes, two or three notifications) per pass of its main loop, and USB flow control holds the rest back until then, so the bank change above is applied over about 8 passes, a few milliseconds. Each notification costs the device roughly 10 µs (an estimate for the Code 61). There's no need to pace notifications for the device's sake.
- **Serial and BLE** (ESP builds): no flow control. The device's receive buffer holds 512 bytes, so keep a burst below that.
- **Redraw:** LEDs and displays are redrawn every 10 ms, so of two states sent to one LED within 10 ms only the last shows.
- **The other way:** read the port promptly. On USB, a host that leaves the device's output unread for 50 ms makes the device drop what it has queued until the host reads again (on ESP builds, what doesn't fit its 1 KB queue). EVENTs in it are lost, and a lost reply means a resend.

**What a script should do:**
- **Change-only** (§5.7): a cache of what was last sent, per LED, value, ring and display row.
- **Coalesce per DAW tick.** Collect changes during one DAW callback or timer tick and send each control's latest state once. 25–50 updates a second per control is plenty for values and rings; faster than one per 10 ms can't show.
- **Send only what the device can show.**
  - FB_VALUE: a device with 7-bit CCs uses `value >> 7`, so a change that leaves the top 7 bits alone does nothing.
  - FB_RING: the ring has `ring` LEDs (§4.4). Current firmware lights them from `p = (value × (ring − 1) + 8191) / 16383` for dot, boost/cut and wrap, and from `w = value × ⌊ring / 2⌋ / 16383` for spread (integer division). When `p` (or `w`), `mode` and `flags` are unchanged, the LEDs are too, so a 15-LED ring needs at most 15 updates over its whole range.
- **Batch:** FB_LEDS for runs of consecutive LEDs (14 + n bytes instead of 14 × n); one FB_TEXT per row, or per changed run of characters, not per character.
- **Text:** a display that follows a moving knob (role `param`) needs at most about 25 updates a second; always send the last one.
- **Requests** are one at a time (§2.3), so connecting costs round trips: HELLO, one GET_COMPONENT per component plus one per kind with none, GET_PARAM and SET_PARAM `debug.events`. The Code 61 has 202 components: about 206 round trips and 9 KB of replies; the Keystation Mini 32 about 21. **Send the next request from the reply handler**, not on the next timer tick: at a few milliseconds per round trip the Code 61 connects in about a second, while one request per 100 ms tick would take 20 s.
- **EVENT rates to expect:** a key, button or pad press is one EVENT. An encoder sends at most one per pass of the device's main loop, with every step since the last, so at most one per detent. An analog input sends at most one per millisecond, only when its 7-bit value changes: at most 128 over its full travel, a few hundred a second while it moves fast. A script that applies them per DAW tick sums an encoder's deltas and keeps the last value of each analog input.

## 6. Roles

The vocabulary lives in [`hosts/README.md`](../hosts/README.md); the builder accepts these names, plus `x_<anything>` for a script's own use and `<host>.<command>` (below). A script **must ignore** roles it doesn't know or doesn't support, logging each one once.

| Group | Roles | On a button / LED / pad | On an analog / encoder |
|---|---|---|---|
| Transport | `play`, `stop`, `record`, `loop`, `metronome`, `overdub`, `session_record`, `automation_arm`, `tap_tempo`, `undo`, `redo`, `rewind`, `fast_forward` | press = the action; the LED shows the state (e.g. `record` lit while recording, blinking while armed but stopped). Momentary actions (`stop`, `undo`, `tap_tempo`…) light only while held; `rewind` / `fast_forward` repeat while held | |
| Markers, edit | `marker_prev`, `marker_next`, `marker_add`, `nudge_back`, `nudge_forward`, `zoom_in`, `zoom_out`, `scrub`, `save`, `time_format` | the action. `scrub`: a toggle, lit while on. `time_format`: switches the `time` display between bars/beats and timecode | |
| Automation | `automation_read`, `automation_write`, `automation_touch`, `automation_latch`, `automation_off` | set the selected track's automation mode; lit for the current one | |
| Navigation | `nav_up`, `nav_down`, `nav_left`, `nav_right`, `nav_select`, `nav_back`, `nav_home`, `nav_alt` | move, select, back, home and the alternative action in the host's own menus or browser | |
| | `nav_scroll` | | encoder: scroll the list |
| Modifiers | `shift`, `option`, `control`, `alt` | held: the script changes what other controls do. Lit while held | |
| Selected track | `track_prev`, `track_next`, `track_mute`, `track_solo`, `track_arm` | the action / its state | |
| | `track_volume`, `track_pan`, `send_a`, `send_b` | | the parameter |
| Mixer bank of 8 | `mute_N`, `solo_N`, `arm_N`, `select_N` (N = 1..8), `bank_prev`, `bank_next`, `strip_prev`, `strip_next` | strip N of the current bank. `bank_*` move the bank by 8, `strip_*` by one; both refresh everything | |
| | `volume_N`, `pan_N`, `send_a_N`, `send_b_N` | | strip N's parameter |
| | `knob_N` | | strip N's assignable knob: what the `assign_*` buttons chose |
| Assign and view | `assign_pan`, `assign_send`, `assign_device`, `assign_eq`, `assign_track`, `assign_instrument` | choose what `knob_1`..`knob_8` control; lit for the current one | |
| | `flip`, `view_all`, `view_midi`, `view_audio`, `view_inputs`, `view_buses` | `flip` swaps the knobs and the faders (lit while flipped); `view_*` choose which tracks the bank shows (lit for the current one) | |
| Master | `master_mute`, `master_solo`, `master_select` | the master track's state / select it | |
| | `master_volume`, `cue_volume`, `crossfader` | | the parameter |
| | `tempo` | | encoder: tempo ± |
| Selected device | `device_param_N` (N = 1..8) | | the device's Nth parameter of the current page (with a ring: FB_RING) |
| | `device_prev`, `device_next`, `device_bank_prev`, `device_bank_next`, `device_on` | navigation / on-off state | |
| Session | `scene_launch`, `scene_prev`, `scene_next`, `stop_all_clips` | the action | |
| Clip grid (§5.9) | `clip_T_S` (T, S = 1..8), `scene_N` | pad or button: launch / stop that clip, or that scene; the LED shows its state | |
| | `grid_up`, `grid_down`, `grid_left`, `grid_right` | scroll the grid by one | |
| Host commands | `<host>.<command>` (below) | any kind: the host's own command | |
| Displays (§5.10) | `time`, `track_name`, `param`, `status` | displays (kind 6) only | |
| Meters (§5.8, reserved) | `meter_N` (N = 1..8), `master_meter` | meters (kind 8) only | |

**Host commands.** A role `<host>.<command>` runs one of a host's own commands, e.g. `zynthian.toggle_audio_play`, or `zynthian.zynpot_1` on an encoder (Zynthian's knob 1). Both parts are lower case: `[a-z][a-z0-9]*\.[a-z0-9_]+`. The host names so far: `live`, `bitwig`, `zynthian`.
- A script handles only its own host's namespace. It ignores the others without logging them, as it does `x_` roles.
- A command it doesn't know in its own namespace is an unknown role: ignore it and log it once.

**Modifiers.** `shift`, `option`, `control` and `alt` are held: the script keeps track of whether each is down (EVENT 1 down, 0 up) and changes what other controls do meanwhile. A modifier can also be a device-side button: the `with:` button of a mode toggle, or a mode's `hold:` button. It still sends EVENTs, so the script sees it as well.

**Long and bold presses.** A host that tells short, bold and long presses apart times them in the script, from the `t_ms` of the press and the release EVENTs, with the host's own thresholds. The device sends no press lengths; nothing in the protocol changes.

**Mackie Control names used as roles** (when a component has `mcu:` but no `daw:`) map like this:
- `fader_N` → `volume_N`;
- `master` → `master_volume`;
- `vpot_N` → `pan_N`;
- `rec_N` → `arm_N`;
- `solo_N`, `mute_N`, `select_N` stay as they are;
- `bank_left` / `bank_right` → `bank_prev` / `bank_next`;
- `channel_left` / `channel_right` → `strip_prev` / `strip_next` (until 2026-11-04: `track_prev` / `track_next`);
- `cycle` → `loop`, `click` → `metronome`;
- `play`, `stop`, `record`, `rewind`, `fast_forward`, `undo` stay as they are.

Lower-case roles before matching.

**Layers (button, pad and encoder twins).** A device may give a physical button, pad or encoder a second meaning while one of its modes is active (core/README.md "Button layers"): a *twin*, listed as its own component of the same kind, after the YAML's, with its own role. Its `layer` byte (§4.4) names its mode. Treat it like any other component (§5.11).
- **How modes turn on.** A toggle (a button, maybe with a `with:` button such as Shift), a hold (while a button is held), or a **cycle group**: several modes, exactly one on at a time, and one button steps to the next. Each step is a mode of its own, with its own twins. Layers are independent: a toggle layer, a hold layer and a cycle step can all be on at once. Each change sends EVENT 8 (§4.5).
- **Which twin wins.** When two modes that are on both have a twin of the same control, one takes it (the order is in core/README.md "Button layers"). Only that one sends EVENTs.
- **Example: the Code 61's encoder modes,** a cycle group. Shift + Encoder mode steps the 8 encoders through:

  | Step | E1–E4 | E5–E8 | Bank left / right |
  |---|---|---|---|
  | MIDI (blue) | plain CC 70–73, no role | plain CC 74–77, no role | `bank_prev` / `bank_next` |
  | ZYN (cyan) | `zynthian.zynpot_1..4` | `device_param_1..4` | `device_bank_prev` / `device_bank_next` |
  | DEVICE (green) | `device_param_1..4` | `device_param_5..8` | `device_bank_prev` / `device_bank_next` |
  | PAN (red) | `pan_1..4` | `pan_5..8` | `bank_prev` / `bank_next` |

  The ZYN, DEVICE and PAN encoders are twins with `cc: none`. The display shows the step's name and the Encoder mode LED its colour. Each step can be turned off in the config (PAN is off by default); the cycle skips the ones that are off, and starts at the first one that is on after power-on. The Arm layer (Shift + Button mode) and the pad grid (Shift + Pad octave −) are layers of their own, independent of the cycle.
- **Button twins** have their own LED (for example `b2` with `mute_1` and `b2_arm` with `arm_1`, on the same switch and the same LED pins). Only the live one sends EVENTs; set both LEDs anyway: the device keeps each state and shows the live one.
- **Pad twins** (kind 7, after the pads; id `<pad>_<mode>`) have their own LED too. Only twins send EVENTs (§5.9): while the layer is active the pad sends EVENT 7 with the twin's index and plays no note. Set the twin's LED anyway; the device shows it while the layer is active.
- **Encoder twins** (kind 4, after the encoders; id `<encoder>_<mode>`) have their own CC and role, and no ring. While the layer is active, turns send the twin's CC and EVENTs with the twin's index; FB_VALUE to an absolute twin sets the twin's value. The base's value is kept for when it comes back.
- **Twins with `cc: none`** (encoders) send no CC at all, only EVENTs. They exist for the script: without one, they do nothing.

**Which component drives an LED.** A button or pad with a role and an LED shows that role's state on its LED. An LED component (kind 5) with its own role shows that role's state. If both apply to the same LED, the button's or pad's role wins.

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
- [ ] Enumeration walks every kind it uses from index 0 using `count`; BAD_PARAM = none (older firmware for kinds 7 and 8).
- [ ] Unknown trailing bytes ignored; missing added fields = "not reported" (§4.4); a missing `layer` = 0.
- [ ] EVENT 8 kept from the SET_PARAM `debug.events` = 1 on (the replay may come first), or ignored if the script doesn't use layers (§5.11).
- [ ] Two components with one role both bound (§5.11).
- [ ] `debug.events` remembered, set to 1, restored on exit.
- [ ] BOOTED → re-enumerate and re-send; keep-alive as a fallback (~30 s with BOOTED/added fields seen, ~5 s otherwise).
- [ ] No FB_VALUE to `takeover` 0 analogs or relative encoders; no FB_LED to `owner` 2 LEDs (§5).
- [ ] Change-only output; FB_LEDS for runs.
- [ ] Changes coalesced per DAW tick; nothing the device can't show (FB_VALUE top 7 bits, FB_RING lit LEDs) (§5.12).
- [ ] The next request sent from the reply handler, not a timer tick (§5.12).
- [ ] The MIDI input read promptly; EVENT bursts summed (encoders) or reduced to the last value (analog) per tick (§5.12).
- [ ] Unknown and unsupported roles ignored (logged once); other hosts' `<host>.<command>` roles ignored quietly; `mcu:` names mapped (§6).
- [ ] EVENT types it doesn't use (touch, pad, layer, reserved) ignored (§4.5).
- [ ] Meters: FB_METER rises only, ≤ ~25/s each (§5.8).
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
  - new EVENT types (such as 8, layer) and new unsolicited messages (`0x60–0x7F`) should be ignored;
  - new notifications (FB_METER) are dropped by older firmware.
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
→ GET_COMPONENT kind 7 index 0   ← BAD_PARAM (no pads, or firmware from before pads)
→ GET_COMPONENT kind 8 index 0   ← BAD_PARAM (no meters)
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
