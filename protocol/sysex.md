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
- **Timeouts:** 200 ms for most requests, **2000 ms for FW_BEGIN and FW_END** (page erase, and the full read-back and CRC). A device that needs longer may reply BUSY, and the host resends.
- **REBOOT:** the reply may be lost as the device resets. Hosts treat a REBOOT timeout as success and then wait for the port to reappear.

The flow itself is stop-and-wait. The host sends one request and waits for its reply (`cmd|0x40`, same `seq`). If there's no reply within 200 ms, it resends the same request with the same `seq`. A device that receives a repeated `seq` for the command it just completed replies again without redoing the work. That matters for `FW_DATA`, where the device must not program the same chunk twice.

## 2. Encodings

- **u14:** 2 bytes, 7 bits each, least significant first.
- **u28:** 4 bytes, 7 bits each, least significant first.
- **u32:** 5 bytes, 7 bits each, least significant first (the last byte carries the top 4 bits).
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

Specific cases:
- An unknown protocol `ver`, or a REBOOT `mode` above 1 → BAD_CMD.
- A FW_BEGIN `size` that's zero, not a multiple of 4, or larger than APROM → BAD_ADDR.
- A frame longer than the device's receive buffer → BAD_LEN.

## 4. Commands

"Boot" = bootloader, "App" = application firmware.

| cmd | Name | App | Boot | Request payload | Reply payload (after status) |
|---|---|---|---|---|---|
| 0x01 | HELLO | ✓ | ✓ | – | `role` (0 app, 1 boot), `board_id` u14, `fw_version` u32, `proto_version` (1 byte), `max_chunk` u14 (bytes of unpacked data per `FW_DATA`, 0 in the app), `caps` u14 (bit flags, §4.1), `uid` (12 bytes packed = 14 bytes) |
| 0x10 | REBOOT | ✓ | ✓ | `mode` (0 = app, 1 = bootloader) | – (the reply is sent, then the device reboots after about 50 ms) |
| 0x18 | FW_BEGIN | – (BAD_STATE) | ✓ | `size` u32, `crc32` u32, `board_id` u14, `fw_version` u32 | `max_chunk` u14 (same rule as HELLO) |
| 0x19 | FW_DATA | – | ✓ | `offset` u28, data (packed, 1..`max_chunk` bytes, multiple of 4) | `offset` u28 (echo) |
| 0x1A | FW_END | – | ✓ | – | – (OK, or BAD_IMAGE if the CRC/header check fails) |
| 0x1B | FW_ABORT | – | ✓ | – | – |
| 0x02–0x08, 0x0F | config (GET_SCHEMA, GET/SET_PARAM, GET/SET_BLOB, COMMIT, FACTORY_RESET, LOG_LEVEL) | later | – | reserved; see `research/core/bootloader-design.md` §6 | |

Unsolicited device → host messages (no reply expected):

| cmd | Name | Payload |
|---|---|---|
| 0x60 | LOG | `level` (0 error, 1 warn, 2 info, 3 debug), `t_ms` u28, text (≤ 64 bytes of 7-bit ASCII) |
| 0x61 | EVENT | `type` (1 button, 2 key, 3 pot), `index` u14, `value` u14, `t_ms` u28 |

The app also answers the Universal Identity Request `F0 7E 7F 06 01 F7` with a standard Identity Reply, so generic tools can find it.

### 4.1 Capability bits (`caps`)
- bit 0: firmware upload (`FW_*`)
- bit 1: config (`GET_SCHEMA` …)
- bit 2: LOG messages
- bit 3: EVENT messages

### 4.2 Firmware upload rules (bootloader)
1. `FW_BEGIN`'s `size`, `crc32`, `board_id` and `fw_version` must equal the values in the image header. The `max_chunk` it returns is authoritative for the upload (HELLO's value is informational). It checks `size` (≤ APROM size, multiple of 4) and `board_id`, then **erases APROM page 0 first**, so the app counts as invalid from this point on.
2. The host sends `FW_DATA` chunks **from offset 0x200 upward in order**, then **page 0 (0x000–0x1FF) last**. That's the one out-of-order region allowed. Chunks must not cross a 512-byte page. A page is erased when a chunk arrives at its start (offset % 512 == 0), except page 0, which `FW_BEGIN` already erased.
3. Only offsets inside `[0, size)` within APROM are accepted. The bootloader never writes LDROM, CONFIG or data flash, and its software check rejects them (BAD_ADDR).
4. `FW_END` reads APROM back, checks the CRC over `[0, size)` (with the header's crc field taken as 0) against the `FW_BEGIN` value and the image header, and replies OK or BAD_IMAGE.
5. A power cut at any point leaves page 0 erased or the CRC wrong, so the next power-up stays in the bootloader.

## 5. Image format (`.gfw`)

A `.gfw` file is the raw APROM binary, exactly `image_size` bytes long (padded with 0xFF to a multiple of 4), linked at 0x0, with a 32-byte header at **0x0C0**, right after the vector table (16 core + 32 IRQ entries). `tools/host/mkimage.py` fills the header after linking.

```c
struct gemifi_img_hdr {      // at 0x000000C0, little-endian
  uint32_t magic;            // 'GMFI' = 0x49464D47
  uint16_t hdr_version;      // 1
  uint16_t board_id;         // §7, must be ≤ 0x3FFF so it fits a u14 on the wire
  uint32_t image_size;       // bytes from 0x0, multiple of 4, <= APROM size
  uint32_t crc32;            // CRC-32/ISO-HDLC (zlib crc32) over [0, image_size), this field taken as 0
  uint32_t fw_version;       // major<<24 | minor<<16 | patch
  uint32_t proto_version;    // 1
  uint32_t reserved[2];      // 0xFFFFFFFF
};
```

The bootloader starts the app only if all of these hold: the magic matches, the `board_id` matches, `image_size` ≤ APROM size, the initial SP is in SRAM, the reset vector is odd and inside the image, and the CRC matches.

## 6. Boot handoff and mailbox (NUC1xx)

- **SRAM mailbox:** 16 bytes at `0x2000_0FF0` that neither side initialises: `magic` u32, `magic_inv` u32 (= ~magic), `boot_count` u32, `reason` u32. `reason` values: 0 = none, 1 = REBOOT to bootloader requested over SysEx; others are reserved. The bootloader doesn't act on `reason`; it's for logging. Both the bootloader and the app put their stack top at `0x2000_0FF0`.
- **App "reboot to bootloader":**
  1. Send the REBOOT reply and wait about 50 ms.
  2. Set magic = `'BOOT'` (0x544F4F42) and magic_inv.
  3. Disconnect USB (DRVSE0, about 10 ms).
  4. Unlock REGWRPROT, set ISPCON.BS = 1, then IPRSTC1.CPU_RST.
- **The bootloader stays in update mode if any of these is true:**
  - the mailbox magic is `'BOOT'` with a matching inverse;
  - the **Pitch▼ + Pitch▲** buttons are held at reset (Keystation; active-low, sampled 3 times over about 20 ms);
  - the app is invalid (§5).

  It clears the magic either way. To start the app: unlock, ISPCON.BS = 0, IPRSTC1.CPU_RST.

## 7. IDs

| Board | `board_id` | Chip |
|---|---|---|
| M-Audio Keystation Mini 32 MK3 | 0x0001 | Nuvoton NUC120LC1BN (32 KB APROM, 4 KB LDROM, 4 KB SRAM) |

USB IDs during development: pid.codes **test** PIDs, development use only. Bootloader `1209:0001`, app `1209:0002` (verify against pid.codes' test range before relying on them). Strings: manufacturer "gemifi"; product "gemifi boot (<board>)" or "<board> (gemifi)"; serial = chip UID in hex. Never reuse M-Audio's `0763:3111` or Medeli's `0A67:138F`.

## 8. Per-board constants
The 512-byte page, APROM size and 200 ms timeout above are NUC1xx values. Other chip families will define their own; `max_chunk` (from HELLO/FW_BEGIN) and the board table are the places to carry them. Revisit when a second family is added.
