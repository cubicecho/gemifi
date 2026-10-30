# gemifi

An "ESPHome for MIDI controllers": open firmware for existing MIDI controllers (and new builds), with everything after the first install done over USB from a web page. That covers firmware updates, debug logs and, later, config such as what each button does.

First target: **M-Audio Keystation Mini 32 MK3** (Nuvoton NUC120). Next: M-Audio Code 61 (STM32F103) and the Behringer FCB1010.

## How it fits together

```
┌──────────────────────────────┐
│ config  (data flash)          │  edited from the web page over SysEx
├──────────────────────────────┤
│ app firmware (APROM)          │  replaced over USB, carries a .gfw header
├──────────────────────────────┤
│ bootloader (LDROM, 4 KB)      │  flashed once over SWD; never writable from USB
└──────────────────────────────┘
```

Everything speaks one protocol, **USB MIDI SysEx**, defined in [`protocol/sysex.md`](protocol/sysex.md).

## Layout

| Folder | What's in it |
|---|---|
| [`protocol/`](protocol/) | The SysEx protocol and `.gfw` image format: the contract between all the parts |
| [`bootloader/`](bootloader/) | Bootloaders, one folder per chip family (`nuvoton-nuc1xx/`) |
| [`firmware/`](firmware/) | App firmware and test apps, per chip family |
| [`platform/`](platform/) | Code shared by a family's bootloader and apps (startup, clocks, USB core, flash, board files) |
| [`web/updater/`](web/updater/) | Browser flasher and log viewer (Web MIDI) |
| [`tools/`](tools/) | SWD scripts (dump, flash, restore), host tools (`gemifi.py`, `mkimage.py`), USB probes. **Start with [`tools/README.md`](tools/README.md).** |
| [`research/`](research/) | Notes and analysis: `core/` (design, framework), `chips/` (per chip family), `devices/` (per device) |

## Status (2026-10-30)
- Keystation: probed, wired for SWD (TP1–TP4), stock firmware fully backed up, hardware map and stock bootloader analysed.
- Bootloader, test apps, web updater and tools: first versions in progress, untested on hardware.

Stock firmware dumps and vendor files are kept locally and are **not** committed (see `.gitignore`).
