# M-Audio Code 61 firmware research

Started 2026-10-30. Goal: understand the stock firmware well enough to patch it or replace it with custom firmware, starting with the "mushy" feel of the pads, faders and encoders.

The Code 25, 49 and 61 all ship the same firmware version on the same dates. We analysed the **Code 49 v1.2.1** updater (`~/windows/Install M-Audio CODE49 FirmwareUpdater 1.2.1.zip`). The Code 61 build is very likely the same code with a different key count, but that's unconfirmed.

## Summary

| | |
|---|---|
| MCU | **STM32F103, high-density** (Cortex-M3, 72 MHz). Evidence: 60-IRQ vector table, UART5 in use, USB FS device at 0x40005C00, F1 GPIO map |
| Toolchain | Keil / ARMCC |
| Encryption | **None.** The firmware ships as plain hex text |
| Bootloader | 0x08000000-0x080023FF (9 KB). Not in the updater; stays in place on update |
| App | 0x08002400-0x08018800 (89 KB), SP init 0x200040B0 |
| Settings | Defaults in flash at 0x080155C0, overridden by a 0x35-byte block in EEPROM (per-unit calibration) |

Custom firmware looks very doable: the chip is well supported, the image is unencrypted, and the bootloader can stay as a recovery path.

## How the firmware is packaged

`WiX Burn .exe` → embedded MSI (OLE compound file, 4 KB sectors) → MSZIP cabinet → `FirmwareFile` (hex text) plus `PanelEXE` (the updater GUI).

The decoded `FirmwareFile` is a series of 0x41E-byte update blocks:

```
+0x00  04 1c 6a 04 da 3b 09     header (constant)
+0x07  AA AA                    target address, big-endian, in 256-byte units (addr = 0x08000000 + AAAA*0x100)
+0x09  00 00 00 0a 00 00 00 04 00
+0x12  1024 bytes of flash data
+0x412 0b AA AA 00 04 00 00 00 00 00   trailer (repeats the address)
+0x41C CC CC                    checksum, little-endian
```

**Checksum (solved 2026-10-30):** `(-sum(record[2:-2])) & 0xFFFF`, stored little-endian. That is, negate the 16-bit sum of every byte from offset 2 up to the checksum. It verifies on all 89 data blocks and on every trailing record. It's the same scheme as the Arturia KeyStep updater ([dsgruss notes](https://dsgruss.com/notes/2020/10/02/keystep1.html)).

After the 89 data blocks come 11 more records:
- Ten 18-byte `00 10 6a 04 da 3b 09 01 AA AA …` records for addresses 0x08018800-0x0801AC00, in 0x400 steps. These are probably page erases past the end of the app.
- One full `04 28 …` block at 0x0801B000. It is mostly 0xFF, with a small table at the end (`c0 b0 1b 00 00 40 00 d0 1b …`) that is probably an app-valid/metadata record.

Settings (including the per-unit calibration) live in an "EEPROM" emulated in flash at 0x0801B400-0x0801FBFF, so updates don't touch them. Whether the bootloader also checks the app image itself is still unknown.

**Update protocol:** worked out from the updater exe. It's plain SysEx: a start message `F0 5A 57 6E 28 3C 4E 45 F7`, then each record as 11-byte chunks in ASCII hex, acknowledged with `F0 45 F7`. Full details are in [analysis/update-protocol.md](analysis/update-protocol.md). `scripts/update_protocol.py` checks a file, rebuilds it around a new app image (byte-identical round trip on the stock file), and writes the exact SysEx stream. Still open: how the keyboard enters update mode (the UI just says "turn it off and on").

## Why the controls feel mushy

Full report with disassembly: [analysis/mushy-feel.md](analysis/mushy-feel.md). There is no single bug: every input is slowed down or dulled by its tuning.

1. **Pads.** Velocity is a linear map of the sensor reading that needs ~95% of full scale for 127 (`cmp.w r1,#0xf20` at 0x08009EA0). The note fires on the 3rd sample using the peak of 3, which adds lag. Fast re-strikes get dropped.
2. **Keys.** Two-contact timing in 400 µs ticks. Anything under ~6 ms is clamped to 127 (`subs r5,#0xc` at 0x080121FE). The default curve (curve1) is linear in time, so velocity changes little at the fast end.
3. **Faders.** An idle fader ignores 160 counts (~5 CC steps) of movement (`movs r4,#0xa0` at 0x08002A2E). Each value is a 4-sample block average.
4. **Encoders.** Acceleration is on by default (±3/5/7 jumps). The first step after a reversal within 80 ms is thrown away.
5. **Wheels and aftertouch.** ±10-count hysteresis gives ~2-step jumps. Aftertouch is muted for 60 ms after the first key goes down.

Each fix is a change of a byte or two. Candidate patches are listed in the analysis. **Nothing has been patched or flashed yet.**

## Velocity tables

`tables/velocity_tables.csv` (kept locally, gitignored: the values come from the vendor image). There are two identical banks of 7 × 128-byte tables (curve0-3, then flat 64 / 100 / 127):

- Bank A, 0x08014C58: keys (0x080097D0)
- Bank B, 0x08014FD8: pads (0x08009E48)

Curve settings live in RAM at 0x20003CA0 (keys) and 0x20003CA1 (pads). Both default to 2, which selects curve1.

## Needs hardware

- Main-loop period: scope the mux select lines PC0-PC2, or toggle a GPIO
- Pad sensor voltage on a hard hit (ADC ch0/ch1)
- Real two-contact key times for soft and hard strikes (key matrix on GPIOB 8-15)
- A board photo, to confirm the chip part number and look for an SWD header

## Layout

```
firmware/  FirmwareFile_v1.2.1.hex.txt   as shipped
           update_blocks_v1.2.1.bin      hex decoded (blocks with headers)
           app_v1.2.1_0x08002400.bin     flat app image, load at 0x08002400
vendor/    updater GUI exe (holds the checksum + protocol), release notes PDF
tables/    velocity_tables.csv, raw table region dump from 0x08014B00 (both gitignored)
analysis/  mushy-feel.md, app_v1.2.1_full.s (full disassembly)
scripts/   extract_firmware.py (installer → app image), fwdis.py, xref.py, cabscan.py
```

Setup: `python3 -m venv .venv && .venv/bin/pip install -r scripts/requirements.txt`

- `python scripts/extract_firmware.py <updater.exe> <outdir>` rebuilds the files in `firmware/`.
- `python scripts/fwdis.py firmware/app_v1.2.1_0x08002400.bin <start> <end>` disassembles a range (hex addresses).

Treat vendor binaries as untrusted. Analyse them statically and never run them.

## Sources

- [Keystation 61 main chip: C8051F320](https://getsatisfaction.com/m-audio/topics/keystation-61-pcb-chip-problem) (related model)
- [Archived Code 61 firmware page](http://web.archive.org/web/20240915200134/https://m-audio.com/support/download/firmware/code-61-firmware-update-for-windows-v1.3.0). The newest is fw 1.0.4.4 / updater 1.3.0 (2018); the download link is now dead
- [M-Audio Japan Code 49/61 firmware page](http://m-audio.jp/code61/data/firmware_update.php)
