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
| `7D` | MIDI "non-commercial / development" ID. Not allowed in products released to the public; make it a build constant so a real manufacturer ID can replace it. |
| `67 6D` | Project signature, ASCII `g` `m`. |
| `ver` | Protocol version, `1`. |
| `cmd` | Request `0x00–0x1F`. Reply = `cmd | 0x40` (so `0x40–0x5F`). Unsolicited device → host messages `0x60–0x7F`. The three ranges never overlap. |
| `seq` | 0–127, chosen by the host and echoed in the reply. Unsolicited messages use a running device counter (not meaningful to the host). |
| `len_lo len_hi` | Payload length in **SysEx bytes** (after packing), 14-bit, low 7 bits first. |
| `chk` | `(sum of bytes from ver through the last payload byte) & 0x7F`. |

Every byte between `F0` and `F7` is 7-bit.

**Flow:** stop-and-wait. (Command numbers were renumbered on 2026-10-30, before any implementation shipped: FW_* moved from 0x20–0x23 to 0x18–0x1B so replies can't collide with LOG/EVENT.)

**Retries:**
- On a timeout, BAD_CHECKSUM or BUSY, the host resends the same request with the **same** `seq`, up to 5 resends (6 sends in total).
- The device only counts a request as *completed* (and so a same-`seq` repeat as a duplicate) after it has replied OK or a final error. A BUSY or BAD_CHECKSUM reply doesn't complete it.
- **Timeouts:** 200 ms for most requests, **2000 ms for FW_BEGIN, FW_END, COMMIT and FACTORY_RESET** (page erases, read-back and CRC). A device that needs longer may reply BUSY, and the host resends.
- **REBOOT:** the reply may be lost as the device resets. Hosts treat a REBOOT timeout as success and then wait for the port to reappear.

The flow itself is stop-and-wait. The host sends one request and waits for its reply (`cmd|0x40`, same `seq`). If there's no reply within 200 ms, it resends the same request with the same `seq`. A device that receives a repeated `seq` for the command it just completed replies again without redoing the work. That matters for `FW_DATA`, where the device must not program the same chunk twice.

## 2. Encodings

- **u14:** 2 bytes, 7 bits each, least significant first.
- **u28:** 4 bytes, 7 bits each, least significant first.
- **u32:** 5 bytes, 7 bits each, least significant first (the last byte carries the top 4 bits).
- **s32:** a signed 32-bit value sent as the u32 of its two's-complement bit pattern (so −1 is `7F 7F 7F 7F 0F`).
- **Packed binary (8→7):** each group of up to 7 input bytes becomes 1 + n output bytes. The first output byte holds the MSBs: bit *i* = bit 7 of input byte *i*. The next *n* bytes are the inputs with bit 7 cleared. A final group of n < 7 bytes produces n + 1 bytes. (This is the Korg/Elektron scheme.)
- **Text:** 7-bit ASCII, unpacked, no terminator. The length comes from the frame.

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

Specific cases:
- An unknown protocol `ver`, or a REBOOT `mode` above 1 → BAD_CMD.
- A FW_BEGIN `size` that's zero, not a multiple of 4, or larger than APROM → BAD_ADDR.
- A frame longer than the device's receive buffer → BAD_LEN.

## 4. Commands

"Boot" = bootloader, "App" = application firmware.

| cmd | Name | App | Boot | Request payload | Reply payload (after status) |
|---|---|---|---|---|---|
| 0x01 | HELLO | ✓ | ✓ | – | `role` (0 app, 1 boot), `board_id` u14, `fw_version` u32, `proto_version` (1 byte), `max_chunk` u14 (bytes of unpacked data per `FW_DATA`; 0 in an app that doesn't take uploads, non-zero in an A/B app), `caps` u14 (bit flags, §4.1), `uid` (12 bytes packed = 14 bytes) |
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
| 0x05, 0x06, 0x0F | GET_BLOB, SET_BLOB, LOG_LEVEL | reserved | – | see `research/core/bootloader-design.md` §6 | |

Unsolicited device → host messages (no reply expected):

| cmd | Name | Payload |
|---|---|---|
| 0x60 | LOG | `level` (0 error, 1 warn, 2 info, 3 debug), `t_ms` u28, text (≤ 64 bytes of 7-bit ASCII) |
| 0x61 | EVENT | `type`, `index` u14, `value` u14, `t_ms` u28. Types: 1 button (value 1 down, 0 up), 2 key (value = velocity, 0 = release), 3 analog: pot, slider, fader, expression input (value = raw reading scaled to 0..16383), 4 encoder (value = signed delta, 14-bit two's complement). Other types are reserved; hosts show them as numbers. |
| 0x62 | PARAM_CHANGED | `id` u14, `index` u14, `value` s32. Sent when the device changes a param by itself (e.g. the octave buttons change `midi.octave`), so an open editor can follow. Not sent for changes the host made itself (SET_PARAM, SET_PARAMS, FACTORY_RESET); after FACTORY_RESET the host reloads. |

The app also answers the Universal Identity Request `F0 7E 7F 06 01 F7` with a standard Identity Reply, so generic tools can find it.

### 4.1 Capability bits (`caps`)
- bit 0: firmware upload (`FW_*`)
- bit 1: config (`GET_SCHEMA` …)
- bit 2: LOG messages
- bit 3: EVENT messages

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
- **GET_PARAM / SET_PARAM** address one value. `index` is 0 for plain params, `0..count-1` for array params (e.g. one action per button).
  - SET_PARAM takes effect **immediately** (live), but is only stored by COMMIT.
  - Unknown `id`, `index >= count`, or a READ_ONLY param → BAD_PARAM. A value outside `min..max` → BAD_VALUE (it is not clamped).
- **COMMIT** writes every non-VOLATILE value to data flash. The stored profile is loaded at power-up. Params missing from the stored profile (for example ones added by a newer firmware) get their defaults, and stored ones the firmware no longer knows are ignored, so a profile survives firmware updates.
- **FACTORY_RESET** sets every value back to its default and erases the stored profile.
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
    option_count × string
```

**Rendering rule.** Array params in the same group with the same `count` and the same index labels form one table: one row per index (labelled by the index label, or by the index), one column per param. That is how a bank of physical controls is described, whatever the device: e.g. `pot.cc[8]`, `pot.channel[8]`, `pot.min[8]`, `pot.max[8]` become an 8-row "Pots" table; `key.note[88]` and `key.velocity_curve[88]` an 88-row one.

**Scope.** The schema describes settings, not the device itself, so the same editor works for a Keystation, a Code 61, an ESP32 build with sliders, encoders and a key matrix, a DIY box or a reverse-engineered legacy unit. Things a future version may add: string values (preset names) through GET/SET_BLOB, a physical layout description for drawing the device, and per-param visibility conditions. A new `type` or a flag bit that adds **no bytes** keeps `schema_version` 1 (old editors show the param as an int and ignore the flag); anything that adds bytes to a param record needs a `schema_version` bump.

Other rules: GET_PARAMS / SET_PARAMS work on plain params too (`index` 0, `n` 1); SET_PARAMS on a READ_ONLY param → BAD_PARAM. Devices never send strings longer than 63 bytes; parsers may accept them. `id`s and `name`s are unique within a schema; bool params have min 0, max 1; bytes after the last param are ignored (room for a future trailer). For GET_PARAMS / SET_PARAMS, an `n` of 0 or above 16 → BAD_LEN.

Unknown `type` values must be shown as plain ints, and unknown `flags` bits ignored, so new firmware keeps working with an older editor. `schema_version` changes only for a layout change an old parser can't skip.

The schema for each firmware is listed in its README (`firmware/<family>/<app>/README.md`).

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

## 6. Boot handoff and mailbox (NUC1xx)

- **SRAM mailbox:** 16 bytes at `0x2000_0FF0` that neither side initialises: `magic` u32, `magic_inv` u32 (= ~magic), `boot_count` u32, `reason` u32. `reason` values: 0 = none, 1 = REBOOT to bootloader requested over SysEx, 2 = the bootloader stayed because of the boot-loop guard; others are reserved. The bootloader doesn't act on `reason`; it's for logging. After a cold start (GCR_RSTSRC POR, LVR or BOD) the bootloader zeroes `boot_count` and `reason`, because SRAM is random then. Both the bootloader and the app put their stack top at `0x2000_0FF0`.
- **App "reboot to bootloader":**
  1. Send the REBOOT reply and wait about 50 ms.
  2. Set magic = `'BOOT'` (0x544F4F42) and magic_inv.
  3. Disconnect USB (DRVSE0, about 10 ms).
  4. Unlock REGWRPROT, set ISPCON.BS = 1, then IPRSTC1.CPU_RST.
- **The bootloader stays in update mode if any of these is true:**
  - the mailbox magic is `'BOOT'` with a matching inverse;
  - the **Pitch▼ + Pitch▲** buttons are held at reset (Keystation; active-low, sampled 3 times over about 20 ms);
  - `boot_count` ≥ 3: the app came back to the bootloader 3 times in a row without clearing it (boot-loop guard; it sets `reason` = 2);
  - the app is invalid (§5).

  It clears the magic either way. To start the app: `boot_count`++, unlock, ISPCON.BS = 0, IPRSTC1.CPU_RST. When it stays, it sets `boot_count` to 0.
- **App side of the guard:** set `boot_count` = 0 after running normally for about 5 s.

## 7. IDs

| Board | `board_id` | Chip |
|---|---|---|
| M-Audio Keystation Mini 32 MK3 | 0x0001 | Nuvoton NUC120LC1BN (32 KB APROM, 4 KB LDROM, 4 KB SRAM) |
| M-Audio Code 61 | 0x0002 | STM32F103-class (possibly a clone), 1 KB flash pages; M-Audio's bootloader stays |
| DIY / private builds | 0x3F00–0x3FFF | any; pick one per device design (examples: 0x3F00 `diy-esp32s3-faders`, 0x3F01 `esp32-example`, 0x3F02 `esp8266-example`) |

USB IDs during development: pid.codes **test** PIDs, development use only. Bootloader `1209:0001`, app `1209:0002` (verify against pid.codes' test range before relying on them). Strings: manufacturer "gemifi"; product "gemifi boot (<board>)" or "gemifi (<board>)"; serial = chip UID in hex. Chips without a UID (the NUC120BN on the Keystation Mini 32) use a random 96-bit ID made on first boot and kept in the last data-flash page (`platform/nuvoton-nuc1xx/src/hal.c`); a factory reset keeps it. The bootloader reads the same record (it never writes it), so both report the same serial; before any app has made the record, the bootloader's serial is all zeros. Never reuse M-Audio's `0763:3111` or Medeli's `0A67:138F`. A device with a DAW port (core/README.md "DAW feedback") has a second USB-MIDI port (cable 1, "<name> DAW") for Mackie Control: this protocol is only spoken on the first port (cable 0), and only there do hosts find the device.

## 8. Per-board constants
The 512-byte page, APROM size and 200 ms timeout above are NUC1xx values. Other chip families define their own; `max_chunk` (from HELLO/FW_BEGIN) and the board table (`tools/host/gemifi_image.py` `BOARDS`) carry them.

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
