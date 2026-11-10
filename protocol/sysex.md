# gemifi SysEx protocol, v1

The single protocol gemifi devices speak over USB MIDI: config, logs and firmware upload.
The bootloader, the app firmware, the web updater (`web/updater`) and the host tool (`tools/host`) all implement this document. Change it here first.

Design background and alternatives: `research/core/bootloader-design.md` §3 and §6.

## 1. Framing

```
F0 7D 67 6D  ver  cmd  seq  len_lo len_hi  payload…  chk  F7
```

| Field | Meaning |
|---|---|
| `7D` | MIDI "non-commercial / development" ID, a placeholder until gemifi has its own (#3). Not allowed in products released to the public. Each implementation keeps it in one constant (`GSX_ID_BYTES` in core, `MANUFACTURER` in `tools/host`, `MANUFACTURER_ID` in `web/updater`). A paid ID is 3 bytes (`00 xx yy`), so the header grows by 2 and every offset after it moves: code finds fields from the ID's length, never from fixed positions. |
| `67 6D` | Project signature, ASCII `g` `m`. |
| `ver` | Protocol version, `1`. |
| `cmd` | Request `0x00–0x1F`. Reply = `cmd | 0x40` (so `0x40–0x5F`). Host → device notifications `0x20–0x3F`, which are never answered (§4.6). Unsolicited device → host messages `0x60–0x7F`. The ranges never overlap. |
| `seq` | 0–127, chosen by the host and echoed in the reply. Unsolicited messages use a running device counter (not meaningful to the host). |
| `len_lo len_hi` | Payload length in **SysEx bytes** (after packing), 14-bit, low 7 bits first. |
| `chk` | `(sum of bytes from ver through the last payload byte) & 0x7F`. |

Every byte between `F0` and `F7` is 7-bit.

**Flow:** stop-and-wait. (Command numbers were renumbered on 2026-10-30, before any implementation shipped: FW_* moved from 0x20–0x23 to 0x18–0x1B so replies can't collide with LOG/EVENT.)

**Retries:**
- On a timeout, BAD_CHECKSUM or BUSY, the host resends the same request with the **same** `seq`, up to 5 resends (6 sends in total).
- The device only counts a request as *completed* (and so a same-`seq` repeat as a duplicate) after it has replied OK or a final error. A BUSY, BAD_CHECKSUM or LOCKED reply doesn't complete it.
- **LOCKED** is not resent at once: the host waits for the user to unlock the device (§4.9), then sends the request again.
- **Timeouts:** 200 ms for most requests, **2000 ms for FW_BEGIN, FW_END, COMMIT, FACTORY_RESET, PRESET_SAVE, PRESET_RESET, PRESET_NAME, PRESET_DELETE and PRESET_MOVE** (page erases, read-back and CRC). A device that needs longer may reply BUSY, and the host resends.
- **REBOOT:** the reply may be lost as the device resets. Hosts treat a REBOOT timeout as success and then wait for the port to reappear.

The flow itself is stop-and-wait. The host sends one request and waits for its reply (`cmd|0x40`, same `seq`). If there's no reply within 200 ms, it resends the same request with the same `seq`. A device that receives a repeated `seq` for the command it just completed replies again without redoing the work. That matters for `FW_DATA`, where the device must not program the same chunk twice.

## 2. Encodings

- **u14:** 2 bytes, 7 bits each, least significant first.
- **u28:** 4 bytes, 7 bits each, least significant first.
- **u32:** 5 bytes, 7 bits each, least significant first (the last byte carries the top 4 bits).
- **s32:** a signed 32-bit value sent as the u32 of its two's-complement bit pattern (so −1 is `7F 7F 7F 7F 0F`).
- **Packed binary (8→7):** each group of up to 7 input bytes becomes 1 + n output bytes. The first output byte holds the MSBs: bit *i* = bit 7 of input byte *i*. The next *n* bytes are the inputs with bit 7 cleared. A final group of n < 7 bytes produces n + 1 bytes. (This is the Korg/Elektron scheme.)
- **Text:** 7-bit ASCII, unpacked, no terminator. The length comes from the frame.
- **string:** `u8 len` (0..16), then `len` bytes of printable ASCII (0x20–0x7E), unpacked. Used for preset names (§4.5). It is not the schema's string (§4.4), which is inside packed data and allows 63 bytes.
- **lstring:** the same with `len` 0..31 (printable ASCII, unpacked). Used for component ids, names and roles (§4.6.1). The device cuts longer strings at 31.

## 3. Replies and status codes

A reply's payload always starts with a status byte. **If the status isn't OK, nothing follows it.**

| Code | Name | Meaning |
|---|---|---|
| 0 | OK | |
| 1 | BAD_CHECKSUM | Frame `chk` wrong |
| 2 | BAD_CMD | Unknown command in this role |
| 3 | BAD_LEN | Payload length wrong for the command |
| 4 | BAD_STATE | Not allowed now (e.g. `FW_DATA` without `FW_BEGIN`, or `FW_*` sent to the app) |
| 5 | BAD_ADDR | Offset out of range, misaligned, or out of order |
| 6 | FLASH_ERR | Erase/program failed (ISPFF set) or read-back mismatch |
| 7 | BAD_IMAGE | Final CRC or header check failed |
| 8 | BAD_BOARD | `board_id` doesn't match this device |
| 9 | BUSY | Try again |
| 10 | BAD_PARAM | Unknown param `id`, `index` out of range, or the param is read-only (§4.3) |
| 11 | BAD_VALUE | Value outside the param's `min..max` (§4.3) |
| 12 | LOCKED | Needs the user's OK on the device first (§4.9): the device has opened its unlock window |

Specific cases:
- An unknown protocol `ver`, or a REBOOT `mode` above 1 → BAD_CMD.
- A FW_BEGIN `size` that's zero, not a multiple of 4, or larger than APROM → BAD_ADDR.
- A frame longer than the device's receive buffer → BAD_LEN.

## 4. Commands

"Boot" = bootloader, "App" = application firmware.

| cmd | Name | App | Boot | Request payload | Reply payload (after status) |
|---|---|---|---|---|---|
| 0x01 | HELLO | ✓ | ✓ | – | `role` (0 app, 1 boot), `board_id` u14, `fw_version` u32, `proto_version` (1 byte), `max_chunk` u14 (bytes of unpacked data per `FW_DATA`; 0 in an app that doesn't take uploads, non-zero in an A/B app), `caps` u14 (bit flags, §4.1), `uid` (12 bytes packed = 14 bytes), then in an app built from a device YAML: `node` (§4.1.1) |
| 0x10 | REBOOT | ✓ | ✓ | `mode` (0 = app, 1 = bootloader) | – (the reply is sent, then the device reboots after about 50 ms) |
| 0x18 | FW_BEGIN | A/B only | ✓ | `size` u32, `crc32` u32, `board_id` u14, `fw_version` u32 | `max_chunk` u14 (same rule as HELLO) |
| 0x19 | FW_DATA | A/B only | ✓ | `offset` u28, data (packed, 1..`max_chunk` bytes, multiple of 4) | `offset` u28 (echo) |
| 0x1A | FW_END | A/B only | ✓ | – | – (OK, or BAD_IMAGE if the CRC/header check fails) |
| 0x1B | FW_ABORT | A/B only | ✓ | – | – |
| 0x02 | GET_SCHEMA | ✓ | – | `offset` u28, `max` u14 (unpacked bytes wanted, 1..64) | `total` u28 (schema size in bytes), `offset` u28 (echo), data (packed, 0..`max` bytes; fewer only at the end) |
| 0x03 | GET_PARAM | ✓ | – | `id` u14, `index` u14 | `id` u14, `index` u14, `value` s32 |
| 0x04 | SET_PARAM | ✓ | – | `id` u14, `index` u14, `value` s32 | `id` u14, `index` u14, `value` s32 (the value now in effect) |
| 0x09 | GET_PARAMS | ✓ | – | `id` u14, `index` u14 (first), `n` u14 (1..16) | `id` u14, `index` u14, `n` u14, then `n` × `value` s32 |
| 0x0A | SET_PARAMS | ✓ | – | `id` u14, `index` u14 (first), `n` u14 (1..16), then `n` × `value` s32 | `id` u14, `index` u14, `n` u14 |
| 0x07 | COMMIT | ✓ | – | – | – (OK, or FLASH_ERR) |
| 0x08 | FACTORY_RESET | ✓ | – | – | – (OK, or FLASH_ERR) |
| 0x0B | PRESET_INFO | presets | – | `slot` u14 (0 = the active one) | `slot` u14, `count` u14, `active` u14, `flags` (1 byte), `revision` u14, `stored_revision` u14, `program` s32, `name` string (§4.5) |
| 0x0C | PRESET_SAVE | presets | – | `slot` u14 (1..count) | – (OK, or FLASH_ERR): store the live values into `slot` and make it active (§4.5) |
| 0x0D | PRESET_RESET | presets | – | `slot` u14 (0 = every slot) | – (OK, or FLASH_ERR): drop the slot's stored edits and name |
| 0x0E | PRESET_NAME | presets | – | `slot` u14 (1..count), `name` string | – (OK, or FLASH_ERR): store a new name (empty = back to the firmware's name); a free slot becomes a new preset |
| 0x12 | PRESET_DELETE | presets | – | `slot` u14 (1..in use) | – (OK, BAD_STATE for the last preset, or FLASH_ERR): delete a preset; the later ones move up (§4.5) |
| 0x13 | PRESET_MOVE | presets | – | `from` u14, `to` u14 (both 1..in use) | – (OK, or FLASH_ERR): move a preset to another slot; the ones between shift by one (§4.5) |
| 0x11 | GET_COMPONENT | feedback | – | `kind` (1 byte), `index` u14 | the component's description (§4.6.1) |
| 0x14 | GET_LAYOUT | layout | – | `offset` u28, `max` u14 (unpacked bytes wanted, 1..64) | `total` u28 (layout size in bytes), `offset` u28 (echo), data (packed, 0..`max` bytes; fewer only at the end) (§4.7) |
| 0x15 | MONITOR | monitor | – | `kinds` (1 byte, bit flags; 0 = stop) | `kinds` (1 byte: the ones it will send), `interval_ms` u14, `timeout_ms` u14 (§4.8) |
| 0x16 | UNLOCK | unlock | – | `open` (1 byte: 0 = only report, 1 = also open the unlock window) | `state` (1 byte), `window` (1 byte), `hold` u14, `n` (1 byte), then `n` × lstring (§4.9) |
| 0x05, 0x06, 0x0F | GET_BLOB, SET_BLOB, LOG_LEVEL | reserved | – | see `research/core/bootloader-design.md` §6 | |

"presets" = only in an app with HELLO caps bit 5; "feedback" = only with caps bit 6; "layout" = only with caps bit 8; "monitor" = only with caps bit 9; "unlock" = only with caps bit 10. Any other device replies BAD_CMD.

Unsolicited device → host messages (no reply expected):

| cmd | Name | Payload |
|---|---|---|
| 0x60 | LOG | `level` (0 error, 1 warn, 2 info, 3 debug), `t_ms` u28, text (≤ 64 bytes of 7-bit ASCII) |
| 0x61 | EVENT | `type`, `index` u14, `value` u14, `t_ms` u28. Types: 1 button (value 1 down, 0 up), 2 key (value = velocity, 0 = release), 3 analog: pot, slider, fader, expression input (value = raw reading scaled to 0..16383), 4 encoder (value = signed delta in steps, 14-bit two's complement; accelerated like its CC), 5 touch (index = the analog's; value 1 touched, 0 released; §4.6.1 `touch`), 7 pad (index = the pad's, twins included; value = the hit's peak reading × 4, 1..16380; 0 = release; only pad twins send it, §4.6.1), 8 layer (index = a mode index; value 1 on, 0 off; §4.6.1 `layer`). Other types are reserved; hosts show them as numbers. |
| 0x62 | PARAM_CHANGED | `id` u14, `index` u14, `value` s32. Sent when the device changes a param by itself (e.g. the octave buttons change `midi.octave`), so an open editor can follow. Not sent for changes the host made itself (SET_PARAM, SET_PARAMS, FACTORY_RESET); after FACTORY_RESET the host reloads. |
| 0x63 | PRESET_CHANGED | `slot` u14 (the active preset), `dirty` (1 byte: 1 = unsaved edits, 0 = none). Sent each time the active preset or its dirty flag changes, whoever caused it (§4.5). |
| 0x64 | BOOTED | The fields of a HELLO reply without its status byte: `role`, `board_id` u14, `fw_version` u32, `proto_version`, `max_chunk` u14, `caps` u14, `uid` (packed, 14 bytes), then `node` when the app has one (§4.1.1). Sent once, about 1.5 s after the app starts (once USB is normally up), and only by an app with caps bit 6 (§4.6.3). A host can treat it exactly like a HELLO reply. |
| 0x65 | RAW | `kind`, then the raw readings of that kind (§4.8). Sent only while a host keeps MONITOR running. |
| 0x66 | UNLOCKED | `state` (1 byte, as in UNLOCK: 1 = unlocked; 0 = locked again, or the unlock window closed without an unlock). Only from an app with caps bit 10 (§4.9). |

The app also answers the Universal Identity Request `F0 7E 7F 06 01 F7` with a standard Identity Reply, so generic tools can find it.

### 4.1 Capability bits (`caps`)
- bit 0: firmware upload (`FW_*`)
- bit 1: config (`GET_SCHEMA` …)
- bit 2: LOG messages
- bit 3: EVENT messages
- bit 5: presets (§4.5: PRESET_* commands, PRESET_CHANGED, params 17–19)
- bit 6: host feedback (§4.6: GET_COMPONENT, the FB_* notifications and BOOTED)
- bit 7: gemifi bootloader: REBOOT(1) leads to a gemifi bootloader (role 1). Only meaningful on boards that may instead carry a vendor bootloader (the Code 61: M-Audio's, with its own update protocol). An app sets it when it finds the gemifi bootloader's marker; without it, hosts use the board's vendor path.
- bit 8: physical layout (§4.7: GET_LAYOUT). Only in an app built from a YAML with `layout:`.
- bit 9: raw readings (§4.8: MONITOR and RAW).
- bit 10: physical unlock (§4.9): FW_BEGIN, REBOOT(1) and FACTORY_RESET may reply LOCKED; UNLOCK and UNLOCKED exist.

### 4.1.1 HELLO `node` (node name)

An app built from a device YAML ends its HELLO reply with `node`: `u8 len` (1..31), then `len` bytes of the YAML's `name:` (the node name: lowercase `a-z`, digits and `-`, not starting or ending with `-`), unpacked. Hosts use it to find the device's YAML and to check that an image was built for this device. The bootloader and test apps without a YAML (probe, usb-log) leave it out: their reply ends after `uid`.

Hosts accept a HELLO reply of the base length (no `node`) or longer, and ignore any bytes after the fields they know, so later fields can be added the same way. A `len` of 0, a `len` that runs past the payload, or a name that breaks the rule above counts as "no node name"; the rest of the reply still stands.

**Who takes uploads.** On chips with a separate bootloader (NUC1xx: LDROM) the app answers `FW_*` with BAD_STATE, and the host sends REBOOT(1) first. On chips with A/B app slots (ESP32 OTA partitions, ESP8266 OTA) **the app itself** takes the upload into the inactive slot and switches to it after a good FW_END plus REBOOT(0); there is no gemifi bootloader, and the chip's ROM bootloader (esptool / Web Serial) is only for the first flash and recovery. The host tells them apart by the HELLO `caps` bit 0 (firmware upload) together with `role`: role 0 with bit 0 set means "upload here, no reboot to the bootloader needed". `max_chunk` in HELLO is then non-zero in the app too.

### 4.2 Firmware upload rules (bootloader)
1. `FW_BEGIN`'s `size`, `crc32`, `board_id` and `fw_version` must equal the values in the image header. The `max_chunk` it returns is authoritative for the upload (HELLO's value is informational). It checks `size` (≤ APROM size, multiple of 4) and `board_id`, then **erases APROM page 0 first**, so the app counts as invalid from this point on.
2. The host sends `FW_DATA` chunks **from offset 0x200 upward in order**, then **page 0 (0x000–0x1FF) last**. That's the one out-of-order region allowed. Chunks must not cross a 512-byte page. A page is erased when a chunk arrives at its start (offset % 512 == 0), except page 0, which `FW_BEGIN` already erased.
3. Only offsets inside `[0, size)` within APROM are accepted. The bootloader never writes LDROM, CONFIG or data flash, and its software check rejects them (BAD_ADDR).
4. `FW_END` reads APROM back, checks the CRC over `[0, size)` (with the header's crc field taken as 0) against the `FW_BEGIN` value and the image header, and replies OK or BAD_IMAGE.
5. A power cut at any point leaves page 0 erased or the CRC wrong, so the next power-up stays in the bootloader.

**A/B devices** (HELLO role 0 with the upload cap): rules 2 and 5 are NUC1xx specifics. The host sends chunks **in plain order from offset 0** (page 0 is not special, and there is no 512-byte page rule; chunks are any multiple of 4 up to `max_chunk`). The old slot stays bootable until FW_END has checked the new one. The host pads the file with 0xFF to a multiple of 4; FW_BEGIN `size` is the padded size and the CRC-32 is over the padded file. FW_BEGIN's reply `max_chunk` is authoritative, as in rule 1. An erase inside FW_DATA can take longer than 200 ms on these chips: the device may reply BUSY, and a resend with the same `seq` is always safe. The image is the chip's own format (e.g. an ESP-IDF / Arduino app `.bin`) without a `.gfw` header; and `board_id`/`fw_version` come from FW_BEGIN. FW_END checks the CRC and marks the new slot to boot; the following REBOOT(0) starts it.

### 4.3 Config (app)

The app keeps a **profile**: one value per (param `id`, `index`). Which params exist is described by the **schema**, a small binary blob built into the firmware (§4.4). An editor needs nothing board-specific: it reads the schema, then the values, and renders a form.

- **GET_SCHEMA** reads the schema in windows (the schema can be up to 2^28 bytes, though small devices keep it to a few KB). The host starts at offset 0 and asks for up to 64 bytes at a time until `offset + returned >= total`. An `offset` at or past `total` returns OK with no data. `max` of 0 or above 64 → BAD_LEN.
- **GET_PARAMS / SET_PARAMS** read or write a run of up to 16 values of one array param (`index .. index+n-1`). `index + n > count` → BAD_PARAM. SET_PARAMS checks every value first and applies none if one fails. Editors use them to load and restore large arrays (88 keys, 64 pots…) quickly; a device that replies BAD_CMD to them still works with single GET/SET_PARAM.
- **GET_PARAM / SET_PARAM** address one value. `index` is 0 for plain params, `0..count-1` for array params (e.g. one action per button). On a device with presets, a per-preset param (flag PRESET, §4.4) reads and writes the **active preset's** value (§4.5).
  - SET_PARAM takes effect **immediately** (live), but is only stored by COMMIT.
  - Unknown `id`, `index >= count`, or a READ_ONLY param → BAD_PARAM. A value outside `min..max` → BAD_VALUE (it is not clamped).
- **COMMIT** writes every non-VOLATILE value to data flash. The stored profile is loaded at power-up. On a device with presets, it stores the globals and the active preset (§4.5). Params missing from the stored profile (for example ones added by a newer firmware) get their defaults, and stored ones the firmware no longer knows are ignored, so a profile survives firmware updates.
- **FACTORY_RESET** sets every value back to its default and erases the stored profile. On a device with presets, it erases every stored preset too and loads preset 1.
- Values the device changes by itself are reported with PARAM_CHANGED.

### 4.4 Schema format (v1)

Little-endian binary, sent packed (8→7) by GET_SCHEMA. Strings are `u8 len` (0..63) followed by `len` bytes of 7-bit ASCII.

```
u8   schema_version      1
u8   group_count
u16  param_count
group_count × string     group labels, e.g. "MIDI", "Buttons"
param_count × param:
  u16  id                1..0x3FFF, stable across firmware versions
  u8   type              0 int, 1 bool, 2 enum, 3 note (int 0..127 shown as a note name, MIDI 60 = C4)
  u8   group             index into the group list
  u16  count             1..0x3FFF (index is a u14 on the wire); array params take index 0..count-1
  u8   flags             bit 0 READ_ONLY
                         bit 1 VOLATILE (not stored by COMMIT)
                         bit 2 PER_INDEX_DEFAULT (count defaults follow instead of one)
                         bit 3 INDEX_LABELS (count strings follow the label)
                         bit 4 HAS_UNIT (a unit string follows the label, e.g. "ms", "%")
                         bit 5 PRESET (one value per preset; GET/SET address the
                               active preset, §4.5; adds no bytes)
  i32  min
  i32  max
  i32  default           × count if PER_INDEX_DEFAULT, else × 1
  string name            stable machine key, e.g. "midi.channel" (used for backups)
  string label           display text, e.g. "MIDI channel"
  string unit            only if HAS_UNIT
  count × string         index labels, only if INDEX_LABELS (e.g. "Sustain", "Edit", …;
                         gemifi firmware sends the components' display names, YAML name:)
  if type == enum:
    u8  option_count     option i has value min + i; max must equal min + option_count - 1
    option_count × string an empty one: a value this firmware leaves out (below)
```

**Options left out.** An enum option with an empty label (length 0) is a value the firmware doesn't have, such as a button action for a message kind the device was built without (`core/README.md` "MIDI messages"), so the numbering of the others stays the same everywhere. A SET of it → BAD_VALUE. Editors don't offer it; an older editor shows a blank option.

**Rendering rule.** Array params in the same group with the same `count` and the same index labels form one table: one row per index (labelled by the index label, or by the index), one column per param. That is how a bank of physical controls is described, whatever the device: e.g. `pot.cc[8]`, `pot.channel[8]`, `pot.min[8]`, `pot.max[8]` become an 8-row "Pots" table; `key.note[88]` and `key.velocity_curve[88]` an 88-row one.

**Scope.** The schema describes settings, not the device itself, so the same editor works for a Keystation, a Code 61, an ESP32 build with sliders, encoders and a key matrix, a DIY box or a reverse-engineered legacy unit. Things a future version may add: other string values through GET/SET_BLOB (preset names have their own command, §4.5) and per-param visibility conditions. The physical layout has its own command (§4.7). A new `type` or a flag bit that adds **no bytes** keeps `schema_version` 1 (old editors show the param as an int and ignore the flag); anything that adds bytes to a param record needs a `schema_version` bump.

Other rules: GET_PARAMS / SET_PARAMS work on plain params too (`index` 0, `n` 1); SET_PARAMS on a READ_ONLY param → BAD_PARAM. Devices never send strings longer than 63 bytes; parsers may accept them. `id`s and `name`s are unique within a schema; bool params have min 0, max 1; bytes after the last param are ignored (room for a future trailer). For GET_PARAMS / SET_PARAMS, an `n` of 0 or above 16 → BAD_LEN.

Unknown `type` values must be shown as plain ints, and unknown `flags` bits ignored, so new firmware keeps working with an older editor. `schema_version` changes only for a layout change an old parser can't skip.

The schema for each firmware is listed in its README (`firmware/<family>/<app>/README.md`).

**Config hash.** Firmware made by the gemifi builder has the param `config.hash` (id 39, group "Debug"): int 0..0x7FFFFFFF, READ_ONLY and VOLATILE, whose value (and default) is the builder's hash of the device YAML it was built from (`core/README.md` "Config hash"). A host compares it with a file's hash to tell whether the device runs that file's config. Firmware built before it has no such param: that means "unknown", not "different". Profiles list it like any read-only param, and an import skips it.

### 4.5 Presets (app)

A **preset** is a named set of values for what the controls do: which CC or note they send, on which channel, with which curve. A device with presets sets HELLO caps bit 5. Design background: `research/core/presets-design.md`.

**Slots.**
- Presets are numbered 1..`count`. The firmware fixes `count` (YAML `presets: { slots }`); PRESET_INFO reports it.
- Preset 1 holds the components' own defaults.
- The other presets in the YAML list only how they differ from preset 1. The remaining slots are empty: copies of preset 1, named "Preset N".

**Presets in use.**
- Slots 1..`preset.count` are the presets in use; the slots after them are **free**. A fresh device has the YAML's list in use (at least 1).
- Only the presets in use can be selected: `preset_next` / `preset_prev` step through them (wrapping), `preset_select` on a free slot does nothing, and SET `preset.active` to a free slot → BAD_VALUE.
- A free slot becomes a preset when it is named (PRESET_NAME: a copy of preset 1's firmware values, under that name) or saved into (PRESET_SAVE: the live values). Doing that past the first free slot puts the slots between in use too, as they are.
- **PRESET_DELETE** `slot` removes a preset: each later preset moves up one slot (its values and its name), and the last slot in use is freed (its stored record erased). The presets in use stay 1..`preset.count`, with no gaps. Then:
  - the active preset keeps playing, one slot lower if it moved; if it was the one deleted, the preset now in its slot (or the one before, if it was the last) is selected;
  - unsaved edits of the active preset are dropped (the moves load each preset);
  - PRESET_CHANGED is sent, and `preset.count` and `preset.active` are stored.
  The last preset can't be deleted (BAD_STATE). A free slot or 0 → BAD_PARAM.
- **PRESET_MOVE** `from` `to` moves the preset in `from` to slot `to` (values and name); the presets between shift one slot towards `from`, so the order changes and nothing else does. The active preset keeps playing wherever it ends up (`preset.active` follows it, with PARAM_CHANGED if its slot changed), its unsaved edits are dropped, PRESET_CHANGED is sent and `preset.active` is stored. `from` = `to` does nothing. A slot that isn't in use → BAD_PARAM; a payload other than 4 bytes → BAD_LEN.
- PRESET_RESET 0 (every slot) also puts the YAML's list back in use.
- A store written before `preset.count` existed keeps every slot with stored edits in use: at power-up the count is raised to the highest stored slot.

**Scope.** Every param is either *per preset* (schema flag PRESET, §4.4) or *global*. The firmware fixes which. The rule: the mapping is per preset (CC and note numbers, channel, velocity curve, encoder mode, glide and ramp times, Program Change). The hardware calibration (velocity `fast`/`slow`) and the device settings (`debug.*`) are global.

**The active preset.**
- Only one preset is live at a time. GET_PARAM, SET_PARAM, GET_PARAMS and SET_PARAMS on a per-preset param address the active preset. Globals work as before.
- So an editor that knows nothing about presets keeps working: it edits the active preset.

**Params.**

| id | Name | Scope | Type | Meaning |
|---|---|---|---|---|
| 17 | `preset.active` | global | int 1..`count` | The active preset. A set switches preset (below). Always present on a device with presets. |
| 18 | `preset.program` | per preset | int −1..127 | Program Change sent when the preset is selected; −1 = none. |
| 19 | `preset.dirty` | VOLATILE, READ_ONLY | bool | 1 while the active preset has unsaved edits. |
| 33 | `preset.count` | global, READ_ONLY | int 1..`count` | The presets in use (above). Changed by PRESET_SAVE, PRESET_NAME, PRESET_DELETE and PRESET_RESET 0, and stored with the globals by the device itself. |
| 41 | `preset.bank` | per preset | int −1..16383 | Bank Select sent before the Program Change: CC 0 = bank >> 7, then CC 32 = bank & 127; −1 = none. Only on devices built with bank select (`core/README.md` "MIDI messages"). |

They are in the group "Presets". When `count` is 1, only `preset.active` is there.

**Select** = SET_PARAM `preset.active` = slot.
1. The device releases everything held: Note Off for held keys and note buttons, CC-momentary buttons, bend and ramps.
2. It loads the slot: param defaults, then the slot's YAML values, then its stored edits. Globals don't change.
3. Octave and transpose go back to the preset's values.
4. It sends the preset's Program Change, if `preset.program` ≥ 0, after its Bank Select if `preset.bank` ≥ 0.
5. It sends PRESET_CHANGED.

Rules:
- Setting the slot that is already active reloads it, and drops its unsaved edits.
- A slot outside 1..`count`, or a free one → BAD_VALUE.
- The device doesn't send PARAM_CHANGED for each value that changed. A host that shows values reads them again after a PRESET_CHANGED with a new slot.
- A switch made on the device itself (a preset button) also sends PARAM_CHANGED for `preset.active`, like any change the device makes.

**Unsaved edits (dirty).**
- A host SET_PARAM or SET_PARAMS on a per-preset param sets dirty. A global param doesn't. Changes the device makes itself (the octave buttons) don't either.
- Dirty shows in `preset.dirty`, in PRESET_INFO `flags` bit 1 and in PRESET_CHANGED.
- Switching preset drops the unsaved edits, like on a hardware synth. Editors warn first.
- COMMIT, PRESET_SAVE and a PRESET_RESET that reloads the active preset clear it.

**Storing.**
- **COMMIT** stores the globals (with `preset.active`) and the active preset. A preset is stored as the values that differ from its YAML values, so an untouched preset costs nothing; the globals as the values that differ from their defaults. A record that is already stored as it is isn't written again (flash wear).
- **PRESET_SAVE** `slot` does the same into `slot` ("save as"), then makes `slot` active. The live values stay as they are: they are now that slot's values. Dirty is cleared. The slot keeps its name. `slot` = the active one is the same as COMMIT.
- **`preset.active`** is stored by COMMIT and PRESET_SAVE. A switch made on the device (or by SET) stores nothing, unless the device YAML has `write_to_flash: { last_preset: true }` (core/README.md "Writes the device makes on its own"): then the device stores it once it has been stable for `delay` (10 s by default) and nothing has been played for `idle` (3 s), so flipping through presets doesn't wear the flash and the write doesn't stall the scan mid-performance. Power-up loads the stored active preset with `last_preset`, else preset 1.

**Reset.** PRESET_RESET `slot` drops the slot's stored edits and its stored name, so it is the firmware's preset again. `slot` 0 = every slot. Globals are not touched. If the active preset is among them, it is reloaded, and its unsaved edits go too.

**Names.**
- PRESET_INFO gives the stored name, else the YAML name, else "Preset N".
- PRESET_NAME stores a new name at once (no COMMIT needed), for any slot, active or not. It doesn't change dirty. An empty name goes back to the firmware's name.
- A name is 0..16 printable ASCII characters (0x20–0x7E). Longer, or another character → BAD_VALUE. A `len` that doesn't match the payload → BAD_LEN.

**PRESET_INFO reply.**

| Field | Meaning |
|---|---|
| `slot` u14 | The slot this is about (the active one when the request said 0). |
| `count` u14 | Number of slots. |
| `active` u14 | The active slot. |
| `flags` | bit 0: the slot has stored edits (changed values, a name, or both). bit 1: dirty (only ever set on the active slot). bit 2: the slot is defined in the YAML (0 = an empty slot). bit 3: the slot is one of the presets in use (0 = free); a firmware from before PRESET_DELETE never sets it, and a host then treats every slot as in use. Other bits 0. |
| `revision` u14 | The firmware's presets revision (YAML `presets: { revision }`, 0..16383, default 0). Same in every reply. |
| `stored_revision` u14 | The revision the slot's stored edits were saved under; 0 when bit 0 is clear. |
| `program` s32 | The slot's `preset.program`: −1 = none, else 0..127. For the active slot, its live value. |
| `name` string | The name, as above. |

The request is 2 bytes; any other length → BAD_LEN. A `slot` above `count` → BAD_PARAM.

**Errors.** For all PRESET_* commands: a `slot` above `count`, or 0 for PRESET_SAVE or PRESET_NAME → BAD_PARAM. A wrong payload length → BAD_LEN. A failed flash write → FLASH_ERR.

**PRESET_CHANGED** (0x63) is sent each time the active preset or its dirty flag changes: a switch on the device or by the host, the first edit after a load, a COMMIT or PRESET_SAVE that clears dirty. Unlike PARAM_CHANGED, it is also sent for changes the host caused, so a host may get one right after its own request.

**After a firmware upload.**
- Stored edits survive an upload. They are tagged by param id and applied on top of the new firmware's YAML presets. A value the user never changed takes the new YAML value; a value they changed keeps theirs.
- Every stored preset remembers the revision it was saved under. After an upload, if any slot has stored edits (bit 0) with `stored_revision` ≠ `revision`, the YAML presets have changed since the user's edits. The host then asks:

| Choice | What the host does | Default |
|---|---|---|
| Keep my presets | nothing: the edits apply on top of the new YAML presets | yes |
| Use the firmware's presets | PRESET_RESET 0 (drops every stored edit and name; the globals stay) | no |

- The host never resets without asking.
- It should back up every preset before the upload (below), so either choice can be undone. Nothing changes in the upload path or the `.gfw` header.

**Backups.** There is no command to read a slot that isn't active, so a host reads every preset by switching:
1. PRESET_INFO for each slot. Read the globals, and the active preset's values (unsaved edits included).
2. Select each other slot in use and read its per-preset params (free slots can't be selected, and aren't backed up).
3. Select the first slot again. If it was dirty, write its unsaved edits back, so the device ends as it started.

To restore, for each slot: if it is free, PRESET_NAME it (so it is in use); PRESET_RESET it; if the backup has stored edits for it, select it, set its values, PRESET_SAVE it and set its name. Then PRESET_DELETE the presets in use past the backup's last one, from the end, select the preset that was active and COMMIT. `tools/host/gemifi.py backup` / `restore` do this.

**Button actions.** Presets add three `button.action` values: 13 `preset_next`, 14 `preset_prev`, 15 `preset_select` (`button.number` = the slot). Mackie Control (on devices with a DAW port) moves from 13 to **16**. A profile stored before this change with an MCU button needs that button set again. Editors take the values from the schema's option labels, so nothing else changes for them.

**MIDI messages** (`core/README.md` "MIDI messages"). Values a device has only when built with the message kind: `button.action` 17 program (`button.number` = the program), 18 panic, 19 MMC (`button.number` = the MMC command), 20 SysEx (`button.number` = the string's index); `analog.mode` 3 cc14, 4 nrpn, 5 nrpn14, 6 rpn, 7 rpn14, 8 SysEx (`analog.cc` = the string's index); `encoder.mode` 3 relative sign-magnitude (every device with encoders), 4 cc14, 5 nrpn, 6 nrpn14, 7 rpn, 8 rpn14 (absolute). With NRPN / RPN, `analog.cc`, `encoder.cc` and `layer.encoder.cc` go up to 16383 (the NRPN or RPN number). The options a device doesn't have are empty (§4.4).

### 4.6 Host feedback (app)

**Status: implemented 2026-11-03** (firmware, `tools/host`, `web/updater`, `hosts/ableton-live`, `hosts/bitwig`); not yet run on hardware or in a DAW. Pads, touch and display roles added 2026-11-04 (#21); the `layer` byte and EVENT 8 later that day (encoder modes); meters (kind 8, FB_METER) and motor faders are reserved. **Writing a DAW script?** [`daw-scripts.md`](daw-scripts.md) is the guide built on this section. Layer F3 of the DAW feedback plan (#6, `research/protocols/daw-feedback-plan.md`).

A script in the DAW (a Live Remote Script, a Bitwig extension) drives the device's LEDs, values and displays with these messages. It addresses everything by **component index**, not by MIDI note or CC. That way it works for any gemifi device without knowing its mapping, and a preset that changes CC numbers doesn't break it. A device with this feature sets HELLO caps bit 6.

The script's side of the loop:
1. **Find the device:** HELLO on each port, looking for caps bit 6.
2. **Learn its controls:** GET_COMPONENT for each kind and index (§4.6.1). The `role` of each component tells the script what to connect it to.
3. **Hear presses and moves by index:** SET_PARAM `debug.events` (id 13) = 1 turns on EVENT messages (0x61). Those carry the button, key, analog or encoder index; the control's normal MIDI goes out as well. The device then replays its layers: EVENT type 8, value 1, for each mode that is on (§4.6.1 `layer`).
4. **Drive feedback:** the FB_* notifications below.
5. **Hand back on exit:** FB_RELEASE when the script stops (Live closing, the device unselected).
6. **Start again after a reboot:** when BOOTED arrives (§4.6.3), the device has lost every FB_* state and `debug.events` is back at 0, so the script re-sends its state.

#### 4.6.1 GET_COMPONENT (0x11)

Request: `kind` (1 byte), `index` u14. Kinds use the EVENT type numbers where they overlap: 1 button, 2 key matrix, 3 analog, 4 encoder, 7 pad; and 5 LED, 6 display, 8 meter (reserved).

Reply (after the OK status): `kind`, `index` u14, `count` u14 (how many components of this kind the device has), `id` lstring (the YAML `id:`), `name` lstring (the display name), `role` lstring, then fields by kind:

| kind | Extra fields |
|---|---|
| 1 button | `led` u14 (its LED's index, or 0x3FFF for none), `layer` (1 byte) |
| 2 key matrix | `keys` u14, `first_note` (1 byte) |
| 3 analog | `takeover` (1 byte: 0 none, 1 pick-up, 2 motor (reserved)), `touch` (1 byte: 0 none, 1 sensor (reserved), 2 from movement), `adc_min` u14, `adc_max` u14 (the YAML's calibration, 0..4095; `adc_min` above `adc_max` = reversed) |
| 4 encoder | `ring` (1 byte: LEDs in its ring, 0 = none), `center` u14 (centre LED index, or 0x3FFF), `mode` (1 byte, below), `layer` (1 byte) |
| 5 LED | `colours` (1 byte: 1 single, 2 bicolour, 3 RGB), `owner` (1 byte: 0 host_then_local, 1 host, 2 local), `layer` (1 byte) |
| 6 display | `cols` (1 byte), `rows` (1 byte), `type` (1 byte: 1 characters, 2 segments) |
| 7 pad | `led` u14 (its LED's index, or 0x3FFF for none), `layer` (1 byte) |
| 8 meter (reserved) | `segments` (1 byte), `type` (1 byte: 1 LED bar, 2 encoder ring, 3 display row) |

An `index` past the end replies BAD_PARAM (`count` comes with every OK reply, so the host learns it from index 0). A device with none of a kind, or a `kind` it doesn't know (0, 9 and up; 7 and up in firmware from before pads), replies BAD_PARAM for index 0, so a host walking every kind treats both as "none". No component has index 0x3FFF, so it can mean "none" in the extra fields. A request that isn't 3 bytes replies BAD_LEN. The reply echoes the request's `kind` and `index`. A host ignores bytes after the fields it knows for a kind, so fields can be added later. The LEDs of an encoder's ring (and its centre LED) are ordinary LEDs: they're in the kind-5 list, and FB_LED can set them too.

**Added fields.** `takeover`, `mode` and `owner` were added after the first F3 firmware, as the last field of their kind, `touch` after `takeover` later (2026-11-04), and `layer` after that (on buttons and pads after `led`, encoders after `mode`, LEDs after `owner`). An analog's `adc_min` and `adc_max` came after `touch` (2026-11-05, for the input tester, §4.8). Older hosts ignore them (the rule above), and newer hosts accept a reply without them: a field that isn't there means "not reported", and the host behaves as it did before (it sends FB_VALUE and FB_LED to everything). A missing `layer` is 0.

**`layer`** says whether the component is a layer twin (`core/README.md` "Button layers"): 0 = always there (not a twin); n = a twin in mode n − 1 (the firmware's mode index), live only while that mode is on. The device only sends EVENTs from live components, so a host may ignore layers. Firmware from before the byte has twins but doesn't mark them.

**EVENT type 8 (layer)** reports the modes: index = the mode index, value 1 on, 0 off. Like every EVENT, only while `debug.events` is 1. It is sent when a layer turns on or off: a toggle, a hold, a cycle step (0 for the old step, then 1 for the new one). When a host sets `debug.events` to 1, the device sends EVENT 8 value 1 once for each mode that is on, so the host learns the current state; these may come before the SET_PARAM reply. A mode without twins may send EVENT 8 too; no `layer` matches it.

**`takeover`** is the analog input's YAML `takeover:` (MCU faders default to pick-up). FB_VALUE only does something with 1, and with 2 (motor, reserved: FB_VALUE moves the fader; no firmware has it yet).

**`touch`** is the analog input's YAML `touch:`. With 1 (a touch sensor, reserved: the builder rejects it for now) or 2 (`touch: movement`), the input sends EVENT type 5 while `debug.events` is 1: value 1 touched, 0 released. With 2, "touched" goes just before the first value EVENT of a movement, and "released" once the reading has been still for 300 ms.

**Pads (kind 7).** `count` covers the YAML's pads, then the pad twins (a mode's `layer:`, `core/README.md` "Button layers"; id `<pad>_<mode>`). `role` is the YAML `daw:`; `led` is the pad's LED, or for a twin its twin LED. A base pad plays its note and sends no EVENT. A twin, while its layer is active, takes the physical pad: it sends EVENT type 7 with its own index and plays no note.

**Encoder twins with `cc: none`** send no CC, only EVENTs (their `layer.encoder.cc` is −1).

**Meters (kind 8)** are reserved. No firmware has them; current firmware replies BAD_PARAM for kind 8.

**`mode`** is how the encoder sends: 0 absolute (`encoder.mode` 0, and the absolute 14-bit and (N)RPN modes 4..8), 1 relative two's complement (`encoder.mode` 1), 2 relative offset 64 (`encoder.mode` 2), 4 relative sign-magnitude (`encoder.mode` 3), 3 Mackie Control V-Pot (an encoder with `mcu:`: relative, on the MCU port, whatever `encoder.mode` says). For an encoder twin it's the twin's `layer.encoder.mode`, a global setting. It's the value in effect when the host asks, so it follows SET_PARAM and preset changes; a host that cares re-reads it after a PRESET_CHANGED (a change made by another host, e.g. an editor's SET_PARAM, sends nothing). FB_VALUE only does something with 0, and hosts treat every other value, including ones added later, as relative.

**`owner`** is who drives the LED (`owner:` in `core/README.md` "LED ownership"): the LED's own `owner:`, else `feedback.led_owner`, except that LEDs driven by Mackie Control (a `feedback: { mcu: … }` source, a V-Pot ring or centre) default to 1. FB_LED, FB_LED_RGB, FB_LEDS and FB_RING have no effect on an LED with 2. An LED with 0 that no button uses has no local state, so it behaves like 1.

**`role`** is the YAML `daw:` value of the component (the names gemifi's scripts understand are listed in `hosts/README.md`) (what a script should connect it to, e.g. `play`, `track_volume`, `device_param_3`). Displays have one too; firmware from before 2026-11-04 always sent theirs empty. If there's no `daw:`, it's the component's `mcu:` name (e.g. `fader_1`). It's empty when there's neither. The role vocabulary is the scripts' business, not the protocol's: the device only passes the string on. Lengths over 31 are cut.

#### 4.6.2 Notifications (host → device, 0x20–0x3F)

Framed like requests, but the device **never replies**, not even with an error. A malformed or unknown notification is dropped. `seq` is ignored (hosts may send 0), and a notification doesn't count as a request for spotting resends (§1), so it can't make the device skip the next real request. The host can send them as fast as the link carries them, with no retries. A host that needs to know they arrived can read the state back (GET_PARAM) or watch the device; the feedback itself is idempotent, so the next update fixes a lost one.

Notifications don't take part in stop-and-wait (§1): the host may send them while a request is waiting for its reply. **Hosts send them only to a device with caps bit 6.** Older firmware answers an unknown command with BAD_CMD as `cmd | 0x40`, which for `0x20–0x3F` lands in the unsolicited range (`0x60–0x7F`) and would read as a LOG or EVENT.

| cmd | Name | Payload |
|---|---|---|
| 0x20 | FB_LED | `led` u14, `level` (1 byte, below) |
| 0x21 | FB_LED_RGB | `led` u14, `r`, `g`, `b` (1 byte each, 0..127), `flags` (bit 0 blink) |
| 0x22 | FB_LEDS | `first` u14, `n` (1 byte, 1..64), then `n` × `level`: several LEDs at once (a bank switch) |
| 0x23 | FB_VALUE | `kind` (3 analog, 4 encoder), `index` u14, `value` u14 (0..16383) |
| 0x24 | FB_RING | `encoder` u14, `mode` (0 dot, 1 boost/cut, 2 wrap, 3 spread), `value` u14 (0..16383), `flags` (bit 0 centre LED, bit 1 ring off) |
| 0x25 | FB_TEXT | `display` u14, `row` (1 byte), `col` (1 byte), text (7-bit ASCII, ≤ 64 bytes) |
| 0x26 | FB_RELEASE | `what` (1 byte: 0 everything, 1 LEDs, 2 values, 3 text) |
| 0x27 | FB_METER | reserved: `meter` u14, `level` (1 byte, 0..127), `flags` (bit 0 clip) |

**`level`** (FB_LED, FB_LEDS), as for plain feedback (`core/README.md`): 0 off, 1 blink, 2..127 on. It's on/off only, not a brightness. On a bicolour LED, 2..63 is colour 1 and 64..127 is colour 2, and 1 blinks colour 1. On an RGB LED, any level other than 0 is white (the LEDs are on/off, so there is no brightness). FB_LED_RGB on a single-colour LED lights it if any of `r`, `g`, `b` is non-zero; on a bicolour LED it shows colour 1 if `r` ≥ `g`, else colour 2; on an RGB LED (`colours` 3: one on/off pin per channel) each channel is on if it is at least half the brightest one, so 127/60/0 is red + green (black = off).

**Ownership.** FB_* states are host states, like plain feedback (`owner:` in `core/README.md`), with one difference: they don't time out (`led_timeout` doesn't apply), because the script is the authority while it runs. An LED with `owner: local` ignores them. FB_RELEASE hands the chosen states back to local, as if the host had never set them.

**FB_VALUE:** for an analog input, `value` becomes its pick-up target (when it has `takeover: pickup`), or moves a motor fader (`takeover` 2, reserved); otherwise it's ignored. For an encoder in absolute mode, it becomes the encoder's value, so the next turn continues from there; in a relative mode it's ignored. Both are in the 14-bit range, so a host with finer values than 7-bit CCs keeps them (a device that sends 7-bit CCs uses the top 7 bits; an encoder in a 14-bit mode all 14). The value is over the control's whole range: with `min:` / `max:`, 0 is `min` and 16383 is `max`.

**FB_RING** spreads `value` over the encoder's ring LEDs in the given mode, the same way as Mackie Control's rings do (§1.2 of `research/protocols/host-feedback.md`), but with 14-bit resolution. 0 is the first LED and 16383 the last. Boost/cut fills from the ring's middle LED to the position, so 8192 is "centre". Spread fills both ways from the middle, `value` setting the width (16383 = every LED). `flags` bit 0 lights the centre LED; on an encoder without one it's ignored. Bit 1 turns every ring LED off (nothing mapped), whatever `mode` and `value` say; the centre LED still follows bit 0.

**FB_METER** (reserved) sets a meter (kind 8). The device draws the fall-off itself, so a host sends rises only, at most about 25 a second for each meter. Current firmware drops it, as it drops any unknown notification.

**FB_TEXT** writes characters into a display from (`row`, `col`). Text is printable ASCII (0x20–0x7E); other bytes show as blank. It's clipped at the end of the row (it doesn't wrap), and a `row` or `col` outside the display drops the message. Nothing else on the row changes, so a host clears old text by sending spaces. Characters a display can't show (a 7-segment one) show as blank.

**Out of range:** an index past the last LED, encoder or analog input is ignored. A `kind`, ring `mode` or release `what` the device doesn't know drops the message. Unused `flags` bits are sent as 0 and ignored.

**EVENTs and pick-up:** an analog input's EVENT value is its calibrated position (after `adc_min`/`adc_max`), scaled to 0..16383. With `takeover: pickup`, EVENTs are held back like its CCs until it picks up the host's value. An encoder's EVENT value is the change in steps since the last one: one per detent, or more on a quick turn when the encoder has `acceleration:` (`core/README.md` "Encoder acceleration"), the same steps its CC and V-Pot messages move.

**FB_RELEASE:** 1 (LEDs) covers every LED state a host set, rings included. 2 (values) drops the pick-up targets, so analog inputs send at once again; encoder values stay where they are. For FB_LEDS, only the LEDs that exist are set, and the rest of the message still applies.

**Buffer:** a device with caps bit 6 accepts notification payloads of at least 72 bytes (FB_LEDS with 64 LEDs, FB_TEXT with 64 characters). A longer one is dropped, silently like any other bad notification, so hosts keep within these limits.

**Rates.** Over USB a device takes one 64-byte packet per pass of its main loop and USB flow control holds the rest back, so a burst of notifications is applied over a few passes and nothing is lost. Serial and BLE links have no flow control; the ESP builds buffer 512 received bytes. LEDs and displays are redrawn every 10 ms. Message costs, a bank change's cost and the rates and batching scripts should use: `daw-scripts.md` §5.12.

#### 4.6.3 BOOTED (0x64, device → host)

A device that reboots or is replugged loses every FB_* state and turns `debug.events` off, without the script hearing about it. So an app with caps bit 6 sends one BOOTED message (unsolicited, §4) about 1.5 s after it starts. Its payload is the HELLO reply's, without the status byte, so a host decodes it with the same code and learns the new `fw_version`, `caps` and `node` (an upload may have changed them).

A script that sees BOOTED from its device (same `uid`, or the only device on the port) starts again from step 2: it reads the components again, sets `debug.events` and re-sends every LED, value, ring and text. An editor (`web/updater`) reloads what it shows.

The message isn't guaranteed to arrive: the host may open the port after it was sent (a replug while the DAW is busy), and MIDI drivers drop SysEx on a closed port. A host that must not miss a reboot keeps a cheap fallback, e.g. reading `debug.events` every few seconds: it reads 0 after a reboot.

**Who sends it.** Only an app with caps bit 6, so it reaches only hosts written for §4.6; the bootloader and apps without host feedback never send it. An older host that doesn't know 0x64 ignores it or shows it as an unknown message; it never answers it. A host never asks for BOOTED: to find a device, it still sends HELLO.

### 4.7 Physical layout (app)

A device built from a YAML with `layout:` (`core/README.md` "Layout") describes where its controls are, so a host can draw it and light up the control being moved (the web page's input tester and "find a control"). It sets HELLO caps bit 8. A device without a layout has no GET_LAYOUT and no layout in flash, and hosts show its controls as a plain list.

**GET_LAYOUT (0x14)** reads the layout blob in windows, exactly as GET_SCHEMA reads the schema (§4.3): `offset` u28, `max` u14 (1..64); the reply is `total` u28, `offset` u28, then up to `max` bytes, packed. The host starts at 0 and reads until `offset + returned >= total`. An `offset` at or past `total` returns OK with no data. A `max` of 0 or above 64, or a request that isn't 6 bytes → BAD_LEN.

**Layout format (v1).** Little-endian binary.

```
u8   layout_version      1
u8   width               the drawing's size in units, 1..255
u8   height
u16  item_count
item_count × item:
  u8  len                bytes after this one: 7, plus the shape's extras
  u8  kind               the control: 1 button, 2 key matrix, 3 analog, 4 encoder, 6 display, 7 pad
                         (GET_COMPONENT's kinds, §4.6.1)
  u8  index              its index in that kind (key matrix: the first key of the item)
  u8  shape              0 button, 1 round button, 2 knob, 3 encoder, 4 fader, 5 wheel, 6 pad,
                         7 display, 8 keys, 9 xy pad
  u8  x, y               the top-left corner, in units from the drawing's top left
  u8  w, h               the size, in units
  extras, by shape:
    8 keys: u8 n_keys    keys index .. index + n_keys - 1
            u8 first_note  the note key `index` plays at octave 0, transpose 0
    9 xy:   u8 y_index   the analog input for the Y axis (`index` is the X axis)
```

- Units are a grid of about 5 mm. Hosts scale the drawing to fit and keep its proportions.
- Every index is the component's own (EVENT's and the schema's index labels'). Layer twins are not drawn: they are the same physical control as their base.
- Only physical controls appear. A control the YAML leaves out of `layout:` has no item; hosts list those under the drawing.
- **Keys.** The keys of a `keys` item are drawn by a fixed rule, so every host draws them alike. White keys (notes 0, 2, 4, 5, 7, 9, 11 mod 12) share the item's width equally, left to right, full height. A black key is 0.6 of a white key wide and 0.6 of the height tall, centred on the line between the white keys either side of it; one at either end of the range is centred on the item's edge.
- An `xy` item is one area for two analog inputs: a reading of `index` is the X position, left to right, and of `y_index` the Y position, bottom to top.
- A host ignores bytes after the fields it knows in an item (it moves on by `len`), so items can grow. It draws an unknown `shape` as a button and skips an item whose `kind` it doesn't know. A new `layout_version` is for a change an old parser can't skip.

### 4.8 Raw readings (app)

For calibrating and porting: the readings as the scan sees them, before debounce, `adc_min`/`adc_max` and velocity. A device with this feature sets HELLO caps bit 9. The web page's input tester shows them.

**MONITOR (0x15).** Request: `kinds` (1 byte, bit flags): bit 0 buttons, bit 1 key matrix, bit 2 analog inputs, bit 3 encoders, bit 4 pads; 0 stops. Reply: `kinds` (the requested ones the device has, which it now sends), `interval_ms` u14 (the time between two RAW messages), `timeout_ms` u14. A request that isn't 1 byte → BAD_LEN. Unknown bits are ignored.

- The device stops by itself `timeout_ms` (5000) after the last MONITOR, so a host that closes or crashes doesn't leave it streaming. A host that wants more sends MONITOR again about every 2 s; each one starts the time again and may change the kinds.
- A reboot stops it (BOOTED, §4.6.3).
- MONITOR changes no setting. EVENTs (`debug.events`) are separate, and work alongside.

**RAW (0x65, device → host).** One message per `interval_ms` (20 ms), from the main loop, the chosen kinds in turn, so the stream is at most about 50 short messages a second and never delays the scan or a key's MIDI. A kind with more than 32 values is split over several messages by `first` (a key matrix by `row`). Payload: `kind` (1 byte, EVENT's numbers), then:

| kind | Fields |
|---|---|
| 1 buttons | `first` u14, `n` (1 byte), then ⌈n / 7⌉ bytes: bit *i* of byte *j* = button `first` + 7*j* + *i*, 1 = pressed (the pin's reading, before debounce) |
| 2 key matrix | `row` (1 byte: the first row in this message), `rows` (1 byte: rows in this message), `cols` (1 byte), then ⌈rows × cols / 7⌉ bytes: bit *b* (7 per byte, as for buttons) with *b* = (r − `row`) × cols + c for row *r*, column *c*; 1 = that contact is closed. A matrix with more than 512 contacts is split by rows. |
| 3 analog | `first` u14, `n` (1 byte), then `n` × u14: the ADC reading, 0..4095, before `adc_min`/`adc_max`; 0x3FFF = no reading (an XY pad not touched) |
| 4 encoders | `first` u14, `n` (1 byte), then `n` × u14: the detents counted since power-up, wrapping at 16384 (not accelerated) |
| 7 pads | `first` u14, `n` (1 byte), then `n` × u14: the pressure reading, 0..4095 |

Only the components with pins are sent: no layer twins. A key matrix's rows and columns are its YAML `rows:` and `columns:`, in order; a velocity keybed has two contacts per key (`key_matrix:` in `core/README.md` "Device YAML"). A contact reads 1 when closed, `inverted:` already applied. Hosts ignore a `kind` they don't know, and bytes after the fields they know.

### 4.9 Physical unlock (app)

Chrome asks for SysEx permission once per site, so any page that was allowed once could flash or wipe a device. With physical unlock (HELLO caps bit 10), the requests that can replace the firmware or lose the settings need the user's OK on the device first, as in Vial and ZMK Studio:

- FW_BEGIN (in an A/B app, §4.1.1),
- REBOOT with `mode` 1 (to the bootloader),
- FACTORY_RESET.

Every other request works as before, REBOOT(0) included.

**Flow.**
1. While the device is locked, those requests reply LOCKED (12) and do nothing else. The LOCKED reply opens the device's **unlock window** for 30 s (or restarts it, if it is already open).
2. The device shows the window: its status LED blinks fast (`core/README.md` "Status LED"), and a display shows `HOLD` and the combo's buttons.
3. The user holds the **unlock combo** (YAML `unlock: { combo }`; `unlock: true` takes the `boot_combo` buttons) for its `hold` time (default 2 s) while the window is open. While the window is open, those buttons do nothing else: no MIDI, no action.
4. The device sends UNLOCKED (0x66) with `state` 1. It stays unlocked until it reboots, or until no request has come for 5 minutes; then it sends UNLOCKED with `state` 0.
5. The host sends the request again. A LOCKED reply doesn't complete a request (§1), so the same `seq` and a new one both work.

If the window closes without an unlock, the device sends UNLOCKED with `state` 0, and the host stops waiting and reports that the device wasn't unlocked.

**UNLOCK (0x16)** tells a host what to ask the user. Request: `open` (1 byte): 0 only reports; 1 also opens the window (or restarts it) when the device is locked, for a host that wants the unlock before it starts. Reply, after the status:

| Field | Meaning |
|---|---|
| `state` | 0 locked, 1 unlocked |
| `window` | seconds left in the unlock window, 0 = closed (1..30 while open) |
| `hold` u14 | ms the combo must be held; 0 = a press is enough |
| `n` | buttons in the combo, 1..8 |
| `n` × lstring | the buttons' display names (YAML `name:`, or from the id) |

A request that isn't 1 byte → BAD_LEN; an `open` other than 0 or 1 → BAD_CMD. A host shows, for example, "Hold Pitch down + Pitch up on the device for 2 s".

**A host's side.** On LOCKED: send UNLOCK (`open` 0) for the prompt, show it, and wait for UNLOCKED: `state` 1 → resend the request; `state` 0, or no UNLOCKED within `window` + 5 s → give up. A host that doesn't know LOCKED shows it as an error, so an old host fails safe. `tools/host/gemifi.py` and `web/updater` do this.

**The bootloader has no lock.** Getting into it already takes the boot combo at power-up, an unlocked REBOOT(1), or the boot-loop guard (§6). The guard needs no host: it trips only when the app keeps failing, so it can't be used to flash a working device.

**Opt-in.** The lock is off unless the device's YAML sets `unlock:` (`true`, or `{ combo, hold }`). Off, the feature is left out: caps bit 10 is clear, nothing replies LOCKED and UNLOCK → BAD_CMD. A host checks caps bit 10 (or just handles LOCKED when it comes) rather than assuming either.

### 4.10 Diagnostics (app)

Read-only, VOLATILE params in the group "Debug", for a host to show (`web/updater` shows them with the device info). Every app built from a device YAML has them (firmware from 2026-11-05 on); a host treats a missing one as "not reported". SET_PARAM on them → BAD_PARAM.

| id | Name | Type | Meaning |
|---|---|---|---|
| 65 | `diag.reset_reason` | enum | Why the app started: 0 unknown, 1 power-on, 2 reset pin, 3 software (REBOOT, an update), 4 watchdog, 5 brown-out, 6 crash (a CPU fault or panic, where the chip tells it apart), 7 low-power |
| 66 | `diag.boot_loop` | bool | 1: before this start, the bootloader's boot-loop guard (§6) had stopped the app (it came back 3 times without running 5 s) |
| 67 | `diag.settings` | enum | The stored settings at power-up: 0 loaded, 1 none stored (defaults), 2 some stored values were refused and reset to their defaults, 3 ignored (boot combo held: safe mode), 4 wiped by the factory-reset combo |
| 68 | `diag.scan_max` | int, µs | The longest scan (the timer tick: key matrix, switch matrices, encoders) since power-up; 0 where the platform can't time it |
| 69 | `diag.loop_max` | int, µs | The longest main-loop pass since power-up |
| 70 | `diag.scan_period` | int, µs | The scan period in use: the YAML's `scan_interval`, or more if a scan ran over it |
| 71 | `diag.uptime` | int, s | Seconds since power-up |

Ids 72–74 are kept for more diagnostics.

What each platform can tell apart:

| Platform | `diag.reset_reason` | `diag.boot_loop` |
|---|---|---|
| NUC1xx | power-on (POR), brown-out (BOD, LVR), watchdog, software (SYSRESETREQ, CPU reset), pin. Passed on by the gemifi bootloader (§6); 0 under a bootloader from before 2026-11-05 | yes |
| STM32F1 | power-on (POR/PDR; the chip reports a brown-out as a power-on), low-power, watchdog (IWDG, WWDG), software, pin. Passed on by the gemifi bootloader (§6); under M-Audio's bootloader, RCC_CSR as it finds it | yes, under the gemifi bootloader |
| ESP32 | `esp_reset_reason()`: power-on, pin, software, watchdog, brown-out, crash (panic), low-power (deep-sleep wake) | no (0) |
| ESP8266 | `rst_info`: power-on, pin, software, watchdog, crash (exception), low-power (deep-sleep wake) | no (0) |

## 5. Image format (`.gfw`)

A `.gfw` file is the raw app binary, exactly `image_size` bytes long (padded with 0xFF to a multiple of 4), with a 32-byte header right after the vector table. `tools/host/mkimage.py` fills the header after linking. Where the header sits depends on the platform (§8):

| Platform | Linked at | Vector table | Header at (image offset) |
|---|---|---|---|
| NUC1xx | 0x0 | 16 core + 32 IRQ entries | **0x0C0** |
| STM32F1 (Code 61) | 0x08002400 | 16 core + 60 IRQ entries | **0x130** |

Offsets below are image offsets, so "from 0x0" means from the start of the image.

```c
struct gemifi_img_hdr {      // at image offset 0x0C0 (NUC1xx) or 0x130 (STM32F1), little-endian
  uint32_t magic;            // 'GMFI' = 0x49464D47
  uint16_t hdr_version;      // 1
  uint16_t board_id;         // §7, must be ≤ 0x3FFF so it fits a u14 on the wire
  uint32_t image_size;       // bytes from the image start, multiple of 4, <= the board's limit (§8)
  uint32_t crc32;            // CRC-32/ISO-HDLC (zlib crc32) over [0, image_size), this field taken as 0
  uint32_t fw_version;       // major<<24 | minor<<16 | patch
  uint32_t proto_version;    // 1
  uint32_t reserved[2];      // 0xFFFFFFFF
};
```

The bootloader starts the app only if all of these hold: the magic matches, the `board_id` matches, `image_size` ≤ APROM size, the initial SP is in SRAM, the reset vector is odd and inside the image, and the CRC matches.

On the Code 61 there is no gemifi bootloader. M-Audio's bootloader stays, and it checks its own image checksum instead (§8). The host tools still check the `.gfw` header before they send anything.

## 6. Boot handoff and mailbox (NUC1xx, STM32F1)

- **SRAM mailbox:** 16 bytes that neither side initialises: `magic` u32, `magic_inv` u32, `boot_count` u32, `reason` u32.
  - NUC1xx: at `0x2000_0FF0`. Both the bootloader and the app put their stack top there.
  - STM32F1: at `0x2000_4FF0`, the top 16 bytes of the 20 KB SRAM, above the stack top (`0x2000_4F00`).
  - `reason` values: 0 = none, 1 = REBOOT to bootloader requested over SysEx, 2 = the bootloader stayed because of the boot-loop guard; others are reserved. The bootloader doesn't act on `reason`. The app reads 2 as "started after the boot-loop fallback" (`diag.boot_loop`, §4.8).
  - After a cold start the bootloader zeroes `boot_count` and `reason`, because SRAM is random then. Cold: GCR_RSTSRC POR, LVR or BOD (NUC1xx); RCC_CSR PORRSTF or LPWRRSTF (STM32F1).
  - **Reset flags.** Once it has decided, the bootloader writes 0 to `magic` and the chip's reset flags to `magic_inv`: GCR_RSTSRC (NUC1xx) or RCC_CSR (STM32F1), as it read them before clearing them. The app reads them there for `diag.reset_reason`, since the chip's own register then only shows the hand-off. Bootloaders from before 2026-11-05 write 0 there (unknown).
- **App "reboot to bootloader":**
  - NUC1xx:
    1. Send the REBOOT reply and wait about 50 ms.
    2. Set magic = `'BOOT'` (0x544F4F42) and magic_inv = ~magic.
    3. Disconnect USB (DRVSE0, about 10 ms).
    4. Unlock REGWRPROT, set ISPCON.BS = 1, then IPRSTC1.CPU_RST.
  - STM32F1: the update-flag page at 0x0801FC00 (§8), then SYSRESETREQ. The mailbox `magic` isn't used.
- **The bootloader stays in update mode if any of these is true:**
  - the app asked for it (NUC1xx: the mailbox magic is `'BOOT'` with a matching inverse; STM32F1: the update flag);
  - the board's button combo is held at reset (Keystation: **Pitch▼ + Pitch▲**; Code 61: **Up + Down**; active-low, sampled 3 times over about 20 ms);
  - `boot_count` ≥ 3: the app came back to the bootloader 3 times in a row without clearing it (boot-loop guard; it sets `reason` = 2);
  - the app is invalid (§5).

  On NUC1xx it clears the magic either way. To start the app: `boot_count`++, then the hand-off (NUC1xx: unlock, ISPCON.BS = 0, IPRSTC1.CPU_RST; STM32F1: MSP and a jump to the app's reset vector). When it stays, it sets `boot_count` to 0.
- **STM32F1, a non-gemifi app.** The STM32F1 bootloader also starts an app without a gemifi header (M-Audio's stock firmware, §8). It never counts those: they don't clear `boot_count`.
- **App side of the guard:** set `boot_count` = 0 after running normally for about 5 s.

## 7. IDs

| Board | `board_id` | Chip |
|---|---|---|
| M-Audio Keystation Mini 32 MK3 | 0x0001 | Nuvoton NUC120LC1BN (32 KB APROM, 4 KB LDROM, 4 KB SRAM) |
| M-Audio Code 61 | 0x0002 | STM32F103-class (possibly a clone), 1 KB flash pages; M-Audio's bootloader stays |
| DIY / private builds | 0x3F00–0x3FFF | any; pick one per device design (examples: 0x3F00 `diy-esp32s3-faders`, 0x3F01 `esp32-example`, 0x3F02 `esp8266-example`, 0x3F03 `diy-bluepill`, 0x3F04 `diy-esp32s3-pads-ws2812`, 0x3F05 `diy-floorboard`) |

USB IDs during development: pid.codes **test** PIDs, development use only. Bootloader `1209:0001`, app `1209:0002` (verify against pid.codes' test range before relying on them). Strings: manufacturer "gemifi"; product "gemifi boot (<board>)" or "gemifi (<board>)"; serial = chip UID in hex. Chips without a UID (the NUC120BN on the Keystation Mini 32) use a random 96-bit ID made on first boot and kept in the last data-flash page (`platform/nuvoton-nuc1xx/src/hal.c`); a factory reset keeps it. The bootloader reads the same record (it never writes it), so both report the same serial; before any app has made the record, the bootloader's serial is all zeros. Never reuse M-Audio's `0763:3111` or Medeli's `0A67:138F`. A device with a DAW port (core/README.md "DAW feedback") has a second USB-MIDI port (cable 1, "<name> DAW") for Mackie Control: this protocol is only spoken on the first port (cable 0), and only there do hosts find the device.

## 8. Per-board constants
The 512-byte page, APROM size and 200 ms timeout above are NUC1xx values. Other chip families define their own; `max_chunk` (from HELLO/FW_BEGIN) and the board table (`tools/host/gemifi_image.py` `BOARDS`) carry them. A DIY board (0x3F00–0x3FFF) isn't in the table: its build writes the layout (header offset, link address, size limit, SRAM) next to the image as `<name>.board.json`, from its board profile (`platform/<platform>/boards/<board>.json`), and the tools check the image against that.

| board_id | Platform | Linked at | Header at | Max image | Upload |
|---|---|---|---|---|---|
| 0x0001 | NUC1xx | 0x0 | 0x0C0 | 32,768 bytes (APROM) | gemifi bootloader in LDROM (§4.2) |
| 0x0002 | STM32F1 | 0x08002400 | 0x130 | 102,398 bytes (to 0x0801B3FD) | M-Audio's bootloader and update protocol |

**Code 61 (0x0002).** gemifi replaces only the app. The flash map and the vendor protocol are in `research/devices/m-audio-code61/analysis/` (`update-protocol.md`, `custom-firmware-path.md`); `tools/host/maudio_update.py` builds and sends the stream.
- 0x08000000–0x080023FF is M-Audio's bootloader. Nothing ever writes it.
- The app is 0x08002400–0x0801B3FD. The update erases every page up to 0x0801AFFF and programs the ones the image covers. It writes page 0x0801B000 last. That page is 0xFF except the **image checksum** at 0x0801B3FE: the negated 16-bit sum of the bytes 0x08002400–0x0801B3FD (unwritten bytes count as 0xFF), little-endian.
- 0x0801B400–0x0801FBFF holds M-Audio's settings and the per-unit calibration. Updates don't touch it. gemifi settings must stay outside the checksummed range, or every save would break the checksum.
- **Update request.** To get into update mode, the app writes the 8 bytes `55 5A A5 AA 59 95 3A A3` to the start of page 0x0801FC00 (rest of the page 0xFF). The bootloader acts on it at the next power-on. In gemifi, REBOOT(1) does this. It then resets, or asks the user to power-cycle; whether the bootloader also reacts after a soft reset isn't known yet.
- The stock app does the same when it gets the vendor start message `F0 5A 57 6E 28 3C 4E 45 F7`, and answers `F0 15 F7`.
