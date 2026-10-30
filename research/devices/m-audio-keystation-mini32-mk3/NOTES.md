# Keystation Mini 32 MK3: working notes

Plans, procedures and recovery notes for using this keyboard as the first gemifi test target.
Raw measurements and capture results are in `probe-findings.md`; raw logs in `captures/`.

> **Repo layout (2026-10-30):** the SWD and dump scripts mentioned below now live in `tools/swd/nuvoton-nuc1xx/` (see `tools/README.md`), the USB probe is `tools/usb/probe-midi-device.sh`, the bootloader design is `research/core/bootloader-design.md`, and the protocol is `protocol/sysex.md`. `backup/` here is gitignored (stock firmware is copyrighted).

## Hardware summary
- MCU: Nuvoton **NUC120LC1BN** (Cortex-M0, 50 MHz, 32 KB APROM, 4 KB LDROM, 4 KB SRAM, USB FS device, LQFP48). PDID `0x10012008`. Obsolete part.
- Target VDD is about 3.7 V (read on the Bus Pirate's Vpu input), not 5 V.
- Stock firmware: no SysEx, no identity reply, no update interface. Channel and CC settings are set from the keyboard's Edit mode.
- APROM is **not locked**: it can be read over SWD.

## SWD header (TP1 to TP4)
| TP  | Signal  | U1 pin | Bus Pirate |
|-----|---------|--------|------------|
| TP1 | ICE_DAT | 29     | MOSI       |
| TP2 | ICE_CK  | 30     | CLK        |
| TP3 | VDD     | 11     | not connected |
| TP4 | GND     | 12/31  | GND        |

Pin layout with the dot at the bottom left: pins 1 to 12 run along the bottom (left to right), 13 to 24 up the right side, 25 to 36 along the top (right to left), and 37 to 48 down the left side. Pins 29 and 30 are 5th and 6th from the right on the top edge.

**Do not connect Bus Pirate Vpu, +3.3V or +5V.** With Vpu on TP3, the keyboard back-powered the Bus Pirate (its power LED stayed on while unplugged). OpenOCD's SWD mode drives 3.3 V push-pull and ignores open-drain, so pull-ups do nothing anyway.

**Power order:** plug in the Bus Pirate first, then the keyboard. Unplug the keyboard first.

## Tooling
- OpenOCD 0.12.0 (Ubuntu) with `openocd-buspirate.cfg`. The config pins `-ap-num 0`: the default AP scan made the Bus Pirate stop responding partway through.
- Bus Pirate v3b, firmware 5.10. Slow (about 80 bytes/s for memory dumps, so 32 KB takes about 6.5 min). Every first SWD write gets a WAIT ack and is retried.
- If an OpenOCD run is killed mid-session, the Bus Pirate gets stuck in binary mode. Unplug and replug it, then re-tick it in VirtualBox Devices → USB.
- Quick health check: Bus Pirate terminal at 115200 baud, `v` shows pin voltages. Expect: CLK and MOSI high when the keyboard is powered, the +5V pin at 0 V.
- The HiLetgo FT232RL board can't do SWD (OpenOCD's ft232r driver is JTAG-only). Use it later as a UART debug console (its jumper allows 3.3 V or 5 V).
- Recommended second probe: a Raspberry Pi Pico running debugprobe (CMSIS-DAP), as a faster and more reliable backup for recovery.

## Useful commands
```sh
# chip ID (read-only)
openocd -f openocd-buspirate.cfg -c init -c "mdw 0x50000000" -c shutdown
# lock check: FFFFFFFF = locked, a vector table = unlocked
openocd -f openocd-buspirate.cfg -c init -c "mdw 0x0 4" -c shutdown
# APROM backup
openocd -f openocd-buspirate.cfg -c init -c "dump_image backup/aprom.bin 0x0 0x8000" -c shutdown
```
CONFIG0/1 (0x00300000) and LDROM (0x00100000) are not readable as plain memory. Use the numicro ISP commands (`numicro read_isp`) with the CPU halted.

## Recovery: can SWD always unbrick it?
Almost always:
- ICE_DAT and ICE_CK are dedicated debug pins on the NUC120, so firmware can't take them over.
- A locked chip can be unlocked with a whole-chip erase over SWD. That erases the firmware, so keep the backup.
- Bad CONFIG values (boot source, clock, watchdog) can make the chip look dead. A chip erase resets them to defaults.

Cases that need more:
1. Firmware that sleeps, stops clocks or resets immediately at boot can make SWD attach fail. The fix is connect-under-reset, which needs a wire to the /RESET pin. That pin is not on TP1 to TP4; **find its pin number and a pad before flashing custom code.**
2. Physical damage can't be fixed over SWD: for example, firmware driving two key-matrix lines against each other. Copy the stock firmware's pin setup and never drive matrix row and column lines as outputs at the same time.

### Restore point checklist (before any write)
- [x] APROM, 32 KB, read twice with identical SHA-256 (`backup/aprom-read{1,2}.bin`, `backup/SHA256SUMS`, files made read-only). 2026-10-30.
- [x] LDROM, 4 KB: `backup/ldrom.bin` (two identical ISP passes). Its vectors are SP 0x20000200 and reset 0xF65; it uses up to 0xFF0.
- [x] CONFIG0 = 0xFF4FFF7F, CONFIG1 = 0xFFFFFFFF (`backup/config.txt`). Decoded (NUC100/120 TRM V2.02): **CBS = 0, so it boots from LDROM**; unlocked; CFOSC = internal 22.1184 MHz RC at reset; brown-out detect and reset on at 3.8 V (so VDD is really about 5 V; the Bus Pirate's 3.73 V reading on Vpu was low); XT1 clock filter on.
- [x] Data flash, 4 KB at 0x1F000: `backup/dataflash.bin` (two identical passes). It holds the settings journal (see below).
- [ ] /RESET pin located, and a wire or pad available. **/RESET is U1 pin 46** (NUC100/120 TRM pin table: LQFP48 pin 46, active low, internal pull-up). Its neighbours are pin 44 XT1_OUT, 45 XT1_IN (both go to crystal X1) and 47 PVSS (ground). With the dot at the bottom left it's on the left edge, 3rd from the bottom. Still to do: find an RC or pad it connects to, and solder a wire. For recovery, hold it to GND by hand while OpenOCD connects. Don't use the Bus Pirate AUX/SRST output: it drives 3.3 V, which may be below the reset pin's 0.7*VDD (3.5 V) high threshold and could hold the chip in reset.
- [ ] Second probe (Pico debugprobe) on hand

## Replacement firmware ideas
- TinyUSB has a device-mode `nuc120` port. Start with a minimal USB MIDI device that sends one note, then scan the key matrix (resistor arrays AR1 to AR6 near the ribbon connector look like matrix lines).
- Stock image uses 17,900 bytes (0x0 to 0x45EB); the rest is 0xFF. Likely lookup tables (decreasing from 127, probably time-to-velocity curves): 0x0EAA (648 bytes, 127 to 10), 0x12CF (841 bytes, 127 to 1), 0x4228 (111 bytes, 127 to 1). These are good first targets for feel analysis.
- Reverse-engineer the stock firmware from the APROM dump for the pin map, matrix scan and velocity timing. Reset handler at 0x44F4; initial SP 0x20000400.
- Feel targets found in the probe captures:
  - Pitch bend is 7-bit only (LSB always 0). Send full 14-bit values.
  - Pitch and mod are buttons that ramp over time. Make the ramp speed and curve configurable over SysEx, which is the gemifi runtime-tunable profile idea.
  - Volume knob on CC 2 (factory default or old user setting, unknown). Make it configurable.
  - Velocity curve: medium playing gave 66 to 108. Make the curve tunable.
  - Add a SysEx identity reply and gemifi's self-describing profile.
- 4 KB of SRAM is tight. Keep the TinyUSB config to MIDI only.
- Restore path: flash `backup/aprom-read1.bin` back to APROM (and restore CONFIG) over SWD.

## Settings journal (data flash, 0x1F400 onward)
The stock firmware appends a 16-byte record each time settings are saved, and the last record wins. Each record holds 7 setting bytes, followed by the tag `"Keystatio"`. 25 records were present (2026-10-30). The latest is `02 00 00 02 84 01 00`.

| Byte | Values seen | Likely meaning |
|------|-------------|----------------|
| 0 | 00 to 04, latest 02 | **MIDI channel, zero-based** (02 = channel 3, which matches the user's setting) |
| 1 | 00, 01, 05 | unknown (octave or transpose?) |
| 2 | 00, 01, 02 | unknown (velocity curve?) |
| 3 | 07, then 02 | **Volume knob CC number**: factory CC 7, later changed to CC 2. That answers the earlier "default or setting?" question. |
| 4 to 6 | 84 01 00 | constant (version or flags?) |

Only bytes 0 and 3 are confirmed by matching them to observed behaviour.

## Speed tip
The FTDI latency timer defaults to 16 ms, and every SWD round trip waited on it. Setting it to 1 ms made ISP reads about 20x faster (0.8 to 17 words/s). Installed permanently via `/etc/udev/rules.d/99-ftdi-latency.rules`:
`ACTION=="add", SUBSYSTEM=="usb-serial", DRIVERS=="ftdi_sio", ATTR{latency_timer}="1"`

## Back-powering through the signal wires
With the keyboard powered, the Bus Pirate's power LED stays on even with its USB unplugged: ICE_DAT sits at 5 V and leaks into the Bus Pirate's rail through the pull-up switch chip on MOSI/CLK. Then a Bus Pirate replug doesn't reset it, and it stays stuck. **Always unplug the keyboard first.** A 1 kΩ series resistor on MOSI and CLK would limit the leak.

## Scripts
- `isp-dump.sh <name> <base> <size> <pass>`: resumable, read-only ISP dump in 256-word OpenOCD sessions. Each word is saved with its address.
- `isp-assemble.py <name> <base> <size>`: builds `backup/<name>.bin` only if both passes are complete and identical.
- `isp-read.tcl`: first attempt. It produced a corrupt LDROM copy (`backup/isp/ldrom-bad-4092bytes.bin`, wrong from 0x100AC4). Don't use it.

## Roadmap (user's plan, 2026-10-30)
1. **Minimal USB base / bootloader** (like WLED's base image), so every later firmware update happens over USB with no more SWD. Design: `research/core/bootloader-design.md`.
2. **Minimal firmware**: press a button and a log event comes back over USB (or UART).
3. Both usable from a browser **device flasher** page (Web MIDI / WebUSB / WebHID / Web Serial).
4. Adapt or clone the fcb1010 Web MIDI editor (github.com/cubicecho/fcb1010) so the page can change what a button does and upload the new config.

**Phasing decision (2026-10-30):** phase 1 has no custom bootloader. The app is flashed over SWD and owns the chip, and all config goes over USB MIDI SysEx from a Web MIDI page into data flash (OpenDeck-style; github.com/shanteacontrols/OpenDeck is the reference design). Phase 2 adds a SysEx firmware-update bootloader that uses the same protocol and page, so design the SysEx protocol with room for firmware-chunk messages. A Raspberry Pi Pico running debugprobe is recommended for fast SWD reflashing during development.

Decision: no full decompile of the stock firmware. Only targeted extraction of what a replacement needs: the hardware map (`analysis/hw-map.md`) and the stock bootloader's handoff and update mode (`analysis/ldrom.md`).

## Updating the bootloader itself over USB (future)
The bootloader can't safely rewrite itself, but the NUC120 lets code running in APROM write LDROM (set ISPCON.LDUEN after the REGWRPROT unlock). So:
1. Through the normal bootloader, upload a special **bootloader-updater app** that carries the new LDROM image.
2. The updater first sets CONFIG0.CBS = 1 (boot from APROM), so a power cut mid-update reboots into the updater instead of a half-written bootloader. Note: an erased CONFIG0 (0xFFFFFFFF) also means "boot APROM, unlocked", so even a power cut during the CONFIG write is survivable.
3. It erases and writes LDROM, verifies it against a CRC, then sets CBS = 0 again and reboots into the new bootloader.
4. Then upload the normal app again.
Design for it now: the bootloader reports its version; the image header has a "target" field (app vs bootloader-updater); only the updater app ever sets LDUEN, and normal apps never touch LDROM. SWD stays the last resort.

## 2026-10-30: M1 passed
- `hello-led.gfw` was flashed to APROM with `tools/swd/nuvoton-nuc1xx/flash-aprom.sh` and verified. The stock LDROM handed off to it and it runs: the Oct− LED blinks at 2 Hz, and the Oct+ LED lights while Oct+ is held.
- **Confirmed on hardware:** our startup code, the 12 MHz → 48 MHz clock init, LEDs active-low (PA10/PA14 and PA11/PA15), Oct+ button on PA4 active-low, and the stock LDROM → APROM handoff with our image.
- The keyboard is **currently running hello-led, not the stock firmware**: it doesn't work as a MIDI keyboard. Run `restore-stock.sh <backupdir>` to go back.
- **Wiring lesson:** SWD failed with all-ones acks until the TP1 wire was moved to the Bus Pirate's real MOSI pin (header pin 8, **grey** on the standard cable; CLK is pin 7 purple, GND pin 1 brown). Bus Pirate `v` showing **MOSI H and CLK H** with the keyboard powered is the go/no-go check. The Bus Pirate self-test (`~`, with Vpu–+5V and ADC–+3.3V jumpers) passed, so the probe itself is fine.
- **Next:** M2, flash `firmware/nuvoton-nuc1xx/usb-log/build/usb-log.gfw` the same way; then button presses should show up via `tools/host/gemifi.py log` (tick the keyboard in VirtualBox Devices → USB; it enumerates as 1209:0002).
