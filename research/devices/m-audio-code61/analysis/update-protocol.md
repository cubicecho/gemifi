# Code series update protocol

Worked out on 2026-10-30, purely from static analysis of `vendor/CODE49_FirmwareUpdater_panel.exe` (MSVC 2012 / MFC, 32-bit) and the app image. Nothing was run, and nothing has been tested on hardware. Addresses below are updater virtual addresses, image base 0x400000. Script: `scripts/update_protocol.py`.

## Transport

- **Plain MIDI SysEx through WinMM.** The updater imports only the `midiOut*` and `midiIn*` functions: no HID, no WinUSB, no DFU. So **Web MIDI with SysEx permission can do everything the updater does.**
- **Device detection:** the updater sends the Universal Identity Request `F0 7E 7F 06 01 F7` (built at 0x4214F0). It accepts replies starting `F0 7E xx 06 02` (0x41FADE).
- **The updater is inMusic's shared framework** ("FireHose", `LUpdateEngine`, `LMidiComm`). The strings link to denondj.com. Other inMusic devices (Akai, M-Audio, Denon) probably use the same protocol (unverified).

## Messages

**Start update:** `F0 5A 57 6E 28 3C 4E 45 F7`
- Built at 0x41EA50. ASCII `ZWn(<NE`, with no manufacturer ID.
- Sent once before the records. The updater then waits **10 s** for an acknowledgement (0x4206C1).
- The same message is sent as a **2 s probe** while the UI is in the "Putting device in update mode" state (0x4208C0).

**Records:** see the format below.
- Each record is sent as consecutive SysEx messages of up to **11 record bytes**, written as **uppercase ASCII hex**: `F0` + 22 characters + `F7`. The last chunk of a record can be shorter.
- The updater sleeps 3 ms between messages (0x4201F2). The send loop is at 0x420170, the hex encoder at 0x41E620, the `F0…F7` wrapper at 0x421590.
- After the last chunk of each record, it waits up to **10 s** for an acknowledgement. A timeout fails the update.

**Device replies:** always three bytes, `F0 cc F7` (parsed at 0x41EB50, handled at 0x41FD10).

| cc | Meaning (inferred from handling) |
|---|---|
| `20` | Device is in update mode (raises a state-change notification) |
| `45` (`E`) | OK, signals the "next" event |
| `1A` | OK, signals the same event as `45` |
| `15` (NAK) | Error: signals the failure event |

The difference between `45` and `1A` isn't known yet. One may mean "erase finished" and the other "write finished".

## Update file format

The file `FirmwareFile` is hex text: the whole update stream in one line, already in wire order.

Record layout:
```
+0   LL LL           big-endian length of the rest of the record (record size - 2)
+2   6A 04 DA 3B 09  constant
+7   AA AA           target address / 0x100, big-endian (0x0024 -> 0x08002400)
...  body
-2   CC CC           checksum = (-sum(record[2:-2])) & 0xFFFF, little-endian
```

| Record | Size | Count in v1.2.1 | Contents |
|---|---|---|---|
| Data | 0x41E | 89 (0x08002400-0x08018400) | 9-byte sub-header `00 00 00 0A 00 00 00 04 00`, 1024 data bytes, trailer `0B AA AA 00 04 00 00 00 00 00` (`0B` = write, `00 04` = 4×256 bytes) |
| Erase? | 0x12 | 10 (0x08018800-0x0801AC00) | Seven zero bytes. Probably erases the pages after the app |
| Final | 0x42A | 1 (0x0801B000) | 1024 × `FF`, then sub-commands `0B` (write), `0D`, `0E` (probably finish/verify and reboot) |

There's no checksum over the whole image in the file. The bootloader might still check the app some other way, and we can't tell without dumping it.

## Flash map (from the app)

| Range | Use |
|---|---|
| 0x08000000-0x080023FF | M-Audio bootloader (not in the update file) |
| 0x08002400-0x080187FF | App |
| 0x08018800-0x0801AFFF | Erased by the update. Free |
| 0x0801B000-0x0801B3FF | Written with `FF` by every update. Probably the bootloader's "update pending / app valid" flag page |
| **0x0801B400-0x0801FBFF** | **Settings: an "EEPROM" emulated in flash.** 1 KB pages, offsets below 0x4800 (read 0x08010E1C, write 0x08010E4C, flash controller 0x40022000). Includes the 0x35-byte per-unit calibration block. **Not touched by updates** |

For replacement firmware:
- Link it at 0x08002400.
- Never write 0x08000000-0x080023FF.
- Read the stock settings and calibration from 0x0801B400 on first boot, then keep or migrate them.

## Still open

1. **How the keyboard gets into update mode.** The UI says: *"Turn the device off and on, and then press the NEXT button."* The ReadMe mentions no key combo. The app has no software reset. **Update 2026-11-01:** the app does handle the start message: it writes a request flag to 0x0801FC00 and replies `F0 15 F7`, and the bootloader acts on the flag at the next power-on. The final record also holds an image checksum. See [custom-firmware-path.md](custom-firmware-path.md).
2. **What the start message does.** It may trigger an erase of the app. The 10 s timeout and the `E` reply would fit. **Assume the app may be gone once it is acknowledged.** You can recover by sending the full stock record stream (`update_protocol.py syx`).
3. **The meaning of `0D`/`0E`**, and `45` vs `1A`.

## Safe first hardware test

1. Connect the keyboard and open its MIDI ports (`amidi -l`, or the web editor).
2. Send the Identity Request and record the reply (model, version).
3. Power-cycle the keyboard. Watch MIDI input for an unprompted `F0 20 F7`, and whether the port name or USB ID changes (`lsusb`, `dmesg`).
4. Only then try the start message. Have the stock `.syx` stream and a sender ready, in case the app gets erased.
