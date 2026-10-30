# Keystation Mini 32 MK3: probe findings (2026-10-30)

Raw data: `probe-20261030-132639-ident.txt` (USB descriptors, SysEx), `probe-20261030-controls.txt` (45 s control capture).

## USB
- 0763:3111, bcdUSB 1.11, full speed, bcdDevice 1.11, EP0 max packet 8, no serial string, 64 mA.
- One configuration: AudioControl + MIDIStreaming only (1 embedded IN jack, 1 OUT jack, bulk EP 0x81 / 0x02, 64 bytes). No HID, DFU or vendor interface.

## SysEx
- No reply to Universal Identity Request (device IDs 7F, 00, 10, 4 s wait each).

## What the controls send
Channel 3 is the user's own setting. Full pass in `probe-20261030-controls-full.txt` (90 s, 1800 messages).

- Keys: Note On with velocity; release is Note On velocity 0 (no 0x80). The full pass skipped the lowest key by accident; note 48 showed up in the earlier capture, so all 32 keys work. Velocities 66 to 108 at medium strength.
- Volume knob: CC 2 (not CC 7), full 0 to 127, one step at a time. Unknown whether that is the factory default or an old user setting.
- Sustain button: CC 64, 127 on press, 0 on release. It's momentary, not latching.
- Edit, Octave -, Octave +: send nothing. Octave only shifts later note numbers.
- Pitch bend buttons (< and >): the firmware ramps pitch bend over time while the button is held and ramps back to 8192 on release. Steps are 128 (7-bit); 0 and 16383 (7F 7F) are the ends.
- Mod button: ramps CC 1 up while held (to 127) and back down to 0 on release. Ramps run at the same time as pitch bend ramps.
- No SysEx, program change or other messages.

## Feel notes (targets for replacement firmware)
- Pitch bend has only 7-bit resolution.
- Pitch and mod ramp speeds and curves are fixed in firmware (could be tuned).
- Volume is on CC 2, a nonstandard choice for volume.

## Board (photos in `fixtures/`, 2026-10-30)
- Label: 1010116170, "Keystation Mini 32 MK3", 1828 (probably 2018 week 28), 0925.
- U1: **Nuvoton NUC120LC1BN** (marking `27S2B0362 807AEDA`, photo `fixtures/20261030_134615.jpg`). Cortex-M0 at 50 MHz, 32 KB flash (APROM) plus a 4 KB LDROM for the ISP bootloader, 4 KB SRAM, full-speed USB device, LQFP48. The part is now obsolete.
- X1: an HC-49 crystal (frequency not readable; the NUC120 normally uses 12 MHz for USB).
- TP1 to TP4: four through-hole test points in a row at the top right. Possibly SWD (VDD, SWDIO, SWCLK, GND) or USB test points. Not traced yet.
- Resistor arrays AR1 to AR6 near the keybed ribbon connector, probably key-matrix lines.
- Debug port: Nuvoton ICE is SWD-compatible (pins ICE_CK and ICE_DAT). TP1 to TP4 may be VDD/ICE_DAT/ICE_CK/GND. Check against the LQFP48 pin table in the NUC120 datasheet before tracing; the pin numbers aren't confirmed yet.

## Implications
- Tooling: upstream OpenOCD's `numicro` driver lists NUC120LC1BN by name (ID 0x10012008, 32 KB APROM, 4 KB LDROM, 4 KB data flash). The user has a Bus Pirate v3.6; upstream OpenOCD's `buspirate` driver supports SWD (Ubuntu candidate is 0.12.0, not installed yet). Bus Pirate wiring: SWDIO to MOSI, SWCLK to CLK, plus GND. It is slow but fine for 32 KB. Measure the target VDD first: the NUC120 runs at 2.5 to 5.5 V, and the Bus Pirate drives 3.3 V. A Nuvoton Nu-Link, or a generic CMSIS-DAP/ST-Link probe, should work over SWD.
- Code protection: CONFIG0 bit 1 (LOCK). If the chip is locked, flash reads back as 0xFFFFFFFF, and the only way to unlock it is a full chip erase, which destroys the stock firmware. **Read CONFIG0 first, and write nothing until there is a backup or the loss of the stock firmware is accepted.**
- Replacement firmware: TinyUSB has a device-mode `nuc120` port, so a TinyUSB MIDI image is a realistic first target. 4 KB of SRAM is tight but enough for MIDI.
- Next: confirm the ICE pin numbers, trace TP1 to TP4, connect an SWD probe, and read the IDCODE and CONFIG0 (read-only).

## SWD setup (2026-10-30)
- Datasheet: NuMicro NUC120 Data Sheet V2.03, Figure 3-4 (LQFP48). Pin 29 = ICE_DAT, pin 30 = ICE_CK, 10 = LDO, 11 = VDD, 12 = VSS. Pins 25 to 36 are on one side; pin 1 is at the dot.
- Bus Pirate v3b, firmware 5.10 (r559). OpenOCD 0.12.0 (Ubuntu) includes the `buspirate` driver, and it uses raw-wire binary mode for SWD.
- Target VDD measured at 5 V. The ICE inputs need 0.7*VDD = 3.5 V, so the config uses Bus Pirate open-drain mode with pull-ups fed from the keyboard 5 V on Vpu (the driver supports this in SWD mode).
- `openocd-buspirate.cfg` and `read-only-check.sh` (reads PDID 0x50000000 and CONFIG0/1 at 0x00300000; writes nothing).
- 2026-10-30 first SWD contact: DPIDR 0x0BB11477 (Cortex-M0, as expected). Every first write gets a WAIT ack, which the driver retries. OpenOCD 0.12 scans APs during examine; the Bus Pirate stopped responding at AP#5 about 1.8 s in. The config now pins `-ap-num 0`. Target VDD on Vpu reads 3.73 V (not 5 V).
- Test points traced (continuity, unpowered): TP1 = U1 pin 29 ICE_DAT, TP2 = U1 pin 30 ICE_CK, TP4 = GND. TP3 reads 5 V when powered, so it's taken to be VDD. Unpowered resistance to TP4: TP1 12.5k, TP2 and TP3 11.5k. So TP1 to TP4 is a ready-made SWD header.
- Back-powering: with the Bus Pirate unplugged and the keyboard powered, the BP power LED stayed lit (leaking through Vpu). Vpu removed, pull-ups off (they do nothing in SWD mode anyway). Power order: plug the BP in first, unplug the keyboard first.
- 2026-10-30 SWD read-only results: PDID (0x50000000) = 0x10012008 = NUC120LC1BN (matches the marking). APROM is **not locked**: 0x0 reads `20000400 000044f5 000043fd 000043fd` (initial SP 0x20000400, reset handler at 0x44F4). CONFIG (0x00300000) and LDROM (0x00100000) aren't readable as plain memory; they need the numicro ISP commands (CPU halted).
- Backup: `backup/aprom-read{1,2}.bin`, 32 KB each, identical (SHA-256 9d33f616...1e83), about 395 s per read. 17,900 bytes used.
