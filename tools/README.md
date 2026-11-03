# gemifi tools: first-steps runbook

This is the order to do things in when you first put your own code on the **M-Audio Keystation Mini 32 MK3** (Nuvoton NUC120LC1BN). Each step can be undone.

| Folder | What's in it |
|---|---|
| `swd/nuvoton-nuc1xx/` | SWD scripts (OpenOCD): check the chip, back it up, flash APROM/LDROM, restore stock |
| `host/` | `mkimage.py` (makes `.gfw` images), `gemifi.py` (USB MIDI or serial: hello, log, reboot, flash, config), `maudio_update.py` (Code 61: M-Audio's update protocol), tests |
| `usb/probe-midi-device.sh` | read-only USB/MIDI probe of the stock keyboard (lsusb, identity request, capture) |

All scripts work from any directory. Every script that writes to flash prints what it will do and waits for you to type `yes`, unless you pass `--yes`. **No script writes CONFIG by default.** Only `restore-stock.sh --write-config` does, and it asks you to type `write config` first.

Add `--dry-run` (or set `DRY_RUN=1`) to any SWD script to print the exact `openocd` commands without touching the probe.

Every SWD script takes `--probe buspirate` (the default) or `--probe cmsis-dap` (a Pico running debugprobe). `PROBE=cmsis-dap` does the same; the flag wins over the variable. See §1.

---

## 0. Words used here

- **APROM** (32 KB at 0x0): the main firmware. Stock: the M-Audio app. Later: the gemifi app.
- **LDROM** (4 KB at 0x00100000): a small boot area. Stock: M-Audio's updater. Later: the gemifi bootloader.
- **Data flash** (4 KB at 0x1F000): settings storage.
- **CONFIG0/1** (0x00300000): chip settings. Stock CONFIG0 = `0xFF4FFF7F`: **boot from LDROM** (CBS=0), **unlocked**, internal RC clock, brown-out reset at 3.8 V. We never need to change it.
- Plain memory reads (`mdw`) work for APROM only. LDROM, data flash and CONFIG need **ISP reads** through the flash controller with the CPU halted. The scripts handle this.

## 1. Wiring (Bus Pirate v3.6)

| Bus Pirate | Keyboard test pad | Chip pin |
|---|---|---|
| MOSI | TP1 | ICE_DAT (U1 pin 29) |
| CLK | TP2 | ICE_CK (U1 pin 30) |
| GND | TP4 | GND |
| Vpu, +3.3V, +5V | **not connected** | |

The keyboard is powered from its own USB cable. The Bus Pirate's power supply stays off.

**Raspberry Pi Pico (debugprobe), when you have it:** GP2 (SWCLK) → TP2, GP3 (SWDIO) → TP1, GND → TP4; nothing else. Then add `--probe cmsis-dap` to every script (or set `PROBE=cmsis-dap` once). It should be far faster and steadier than the Bus Pirate.

The Pico path is **not tested on hardware yet**. The scripts pass the right config (`openocd-cmsis-dap.cfg`) to OpenOCD; offline tests check that. What still needs a real run: `check.sh --probe cmsis-dap`, then `read-config.sh`, then `flash-ldrom.sh --verify-only` against the bootloader that's on the chip. The Bus Pirate stays the default until that works.

**The Bus Pirate is flaky on long sessions.** Two things to know:
- SWD writes sometimes drop partway on images of 12 KB or more. Just retry.
- A killed OpenOCD session leaves the Bus Pirate stuck. Replug it (order in §2) before you retry.

## 2. Plug order (important)

The keyboard leaks 5 V into the Bus Pirate through MOSI/CLK (back-powering). Once that happens, unplugging and replugging the Bus Pirate no longer resets it.

- **Plugging in:** plug in the **Bus Pirate first**, then the **keyboard last**.
- **Unplugging:** unplug the **keyboard first**, then the Bus Pirate.
- **If the Bus Pirate stops answering** (an OpenOCD session was killed, timed out or hung), it's stuck in binary mode. Unplug the keyboard, then the Bus Pirate. Plug in the Bus Pirate, then the keyboard. Rerun the command. Dumps pick up where they stopped. A stopped LDROM verify prints its own resume command (§5).
- Run only one OpenOCD at a time. The scripts refuse to start if another one is running.

## 3. One-time setup checks

```sh
cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer     # must print 1
```

If it doesn't print 1, SWD is about 16× slower and long sessions time out. The scripts warn about this. Fix it for now with `echo 1 | sudo tee /sys/bus/usb-serial/devices/ttyUSB0/latency_timer`. The udev rule that makes it permanent looks like this:

```
# /etc/udev/rules.d/99-ftdi-latency.rules
ACTION=="add", SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", ATTR{latency_timer}="1"
```

The config pins the debug access port with `-ap-num 0`. OpenOCD's default AP scan hung the Bus Pirate. Don't use OpenOCD's stock `target/numicro.cfg` with the Bus Pirate.

## 4. First session, step by step

Shorthand: `SWD=tools/swd/nuvoton-nuc1xx` (run from the repo root, or use full paths).

### Step 1: is the chip there? (read-only)

```sh
$SWD/check.sh
```

Expect `PDID: 0x10012008 = NUC120LC1BN OK` and `APROM: readable (chip unlocked)`. If APROM reads all `FFFFFFFF`, **stop** and write nothing: the chip is locked or blank.

```sh
$SWD/read-config.sh          # CONFIG0/1 by ISP read, decoded (halts the CPU for a moment)
```

Expect `CONFIG0 = 0xFF4FFF7F`, `CBS = 0 boot from LDROM`, `LOCK = 1 unlocked`.

### Step 2: take a full backup (read-only)

You already have one in `research/devices/m-audio-keystation-mini32-mk3/backup/`. Never modify or delete it. Take a fresh one with the new script and compare the two:

```sh
$SWD/dump.sh ~/gemifi-backups/kms32-$(date +%Y%m%d) \
    --compare research/devices/m-audio-keystation-mini32-mk3/backup
```

What it does:
- reads APROM twice (two OpenOCD sessions) and compares the reads;
- reads LDROM, data flash and CONFIG by ISP, in two passes of 256 words per session (long Bus Pirate sessions lock up), and assembles them only if both passes agree;
- writes `SHA256SUMS` and makes the files read-only.

On the Bus Pirate this takes about 15 minutes for APROM and 10–20 more for the ISP passes. If it fails partway: replug (keyboard out first), then run the same command with `--resume` added. It refuses to write into a folder that already has files in it, unless that folder came from `dump.sh` and you pass `--resume`.

Expect `same` for every file in the comparison (APROM, LDROM, data flash, CONFIG0). Keep a copy of the backup off this machine. It's copyrighted firmware, so never commit or publish it.

### Step 3: flash milestone M1 (`hello-led`) to APROM

Build the app with the firmware Makefile (see `firmware/`). It produces `build/hello-led.bin` and `build/hello-led.gfw` next to its Makefile. Then:

```sh
$SWD/flash-aprom.sh firmware/nuvoton-nuc1xx/<app dir>/build/hello-led.gfw
```

What happens:
- it checks the `.gfw` header with `mkimage.py --check` (a plain `.bin` gets a size and vector-table check instead);
- prints the plan;
- writes APROM and verifies it;
- resets the chip.

LDROM, data flash and CONFIG aren't touched. The stock LDROM still runs first (CBS=0) and hands off to APROM, so your app starts.

If the write drops partway (common on the Bus Pirate with images of 12 KB or more), replug if needed and run the same command again. To check what's in APROM without writing, use `flash-aprom.sh --verify-only <file>`. It runs `verify_image` in one short session.

Expected result: the Oct− LED blinks, and Oct+ lights while you hold it.

### Step 4: put the stock firmware back

```sh
$SWD/restore-stock.sh ~/gemifi-backups/kms32-YYYYMMDD            # or the research backup folder
$SWD/restore-stock.sh <backup> --with-dataflash                   # also restore stock settings
```

What it does:
- verifies the backup's `SHA256SUMS`;
- writes APROM (then `verify_image`) and LDROM (then an ISP read-back compare), each in its own short session;
- reads CONFIG and compares it with the backup's `config.txt`.

If the LDROM read-back stops, the write is done. Resume just the verify with the command it prints (§5), then run `read-config.sh`. There's no need to rerun the whole restore.

**It never writes CONFIG** unless you pass `--write-config`. Even then it only writes when CONFIG differs: it shows the old and new values decoded and waits for you to type `write config`. It refuses any value that would lock the chip.

Unplug the keyboard (first), plug it back in, and check that it works as a normal MIDI keyboard again (`tools/usb/probe-midi-device.sh`).

Repeat steps 3 and 4 as often as you like. That's the M1/M2 development loop. M3 (bootloader test build in APROM, `bootloader/…/build/test-aprom/gemifi-boot-test.bin`) goes in the same way through `flash-aprom.sh`.

## 4a. Ready-to-test images (built 2026-10-30)

All built and host-tested; none has run on hardware yet. Do them in this order. Each step only needs the one before it. Before each SWD step: keyboard unplugged, Bus Pirate plugged in, keyboard plugged in last, `v` shows MOSI H and CLK H.

| Step | Image | Flash with | What to check |
|---|---|---|---|
| **M2** USB log | `firmware/nuvoton-nuc1xx/usb-log/build/usb-log.gfw` | `$SWD/flash-aprom.sh <gfw>` | Replug (tick the device in VirtualBox). `python3 tools/host/gemifi.py hello`, then `gemifi.py log` and press each button: a LOG and EVENT per press. `gemifi.py reboot boot` resets via the stock LDROM back into the app (look for `mb=544F4F42` in the boot log). |
| **M2b** controller | `firmware/nuvoton-nuc1xx/midi-controller/build/midi-controller.gfw` | `$SWD/flash-aprom.sh <gfw>` | The keyboard plays again with gemifi firmware: keys, velocity, all buttons, knob, LEDs. `gemifi.py get` / `set` / `commit`; the web updater's config card. Checklist in [`firmware/nuvoton-nuc1xx/midi-controller/README.md`](../firmware/nuvoton-nuc1xx/midi-controller/README.md). |
| **M3** bootloader test | `bootloader/nuvoton-nuc1xx/build/test-aprom/gemifi-boot-test.bin` | `$SWD/flash-aprom.sh <bin>` | Shows up as "gemifi boot test (Keystation Mini 32)", Oct LEDs blinking alternately. `gemifi.py hello` → role boot. `gemifi.py flash firmware/nuvoton-nuc1xx/hello-led/build/hello-led.gfw` uploads into **data flash** (images ≤ 4 KB only); OK at FW_END means the whole upload path works. This overwrites the stock settings in data flash (backup restores them). |
| **M4** bootloader | `bootloader/nuvoton-nuc1xx/build/gemifi-boot.bin` | `$SWD/flash-ldrom.sh <bin> --i-have-a-backup <dir>` (§5) | Without an app in APROM it stays in update mode. Then `gemifi.py flash …/midi-controller.gfw` (or the web updater) installs the app over USB, and from then on SWD is only for recovery. |
| **M5** web | `cd web/updater && npm run dev` | – | Connect, HELLO, log, flash, config editor (Chrome/Edge: Web MIDI with SysEx permission). |

Back to stock at any point: `$SWD/restore-stock.sh <backup> --with-dataflash` (step 4).

## 5. Later: bootloader in LDROM (milestone M4)

```sh
$SWD/flash-ldrom.sh bootloader/nuvoton-nuc1xx/build/gemifi-boot.bin \
    --i-have-a-backup ~/gemifi-backups/kms32-YYYYMMDD
```

Guards before anything is written:
- the backup folder must have a `SHA256SUMS` that verifies and a 4096-byte `ldrom.bin`;
- the file must be ≤ 4096 bytes with a sane vector table.

What it does:
1. Prints the recovery steps.
2. Erases all of LDROM and writes the file.
3. Resets the chip.
4. Verifies by ISP read-back. LDROM can't be read as plain memory, so `verify_image` can't be used.

CONFIG isn't touched. With CBS=0 the chip boots your bootloader from now on.

### Verify only, and resuming a verify

The read-back takes a while on the Bus Pirate, and it can stop halfway. When it stops, the write is already done. **Don't rerun the write.** The script prints the exact command that resumes the verify, for example:

```sh
$SWD/flash-ldrom.sh --verify-only bootloader/nuvoton-nuc1xx/build/gemifi-boot.bin \
    --resume tools/swd/nuvoton-nuc1xx/logs/verify-ldrom-20261031-120000-4242 --probe buspirate
```

- `--verify-only <bin>` never erases or writes. It reads LDROM back by ISP and compares it with the file. It needs no backup folder and asks no question.
- `--resume <dir>` reads only the words that aren't in the folder yet. The folder records the file's SHA-256, so a resume against a different file is refused.
- Without `--resume`, `--verify-only` starts a fresh read-back in a new folder.
- If the Bus Pirate is stuck, replug it (§2) before you resume.

One pass is enough here, because the file is the reference. A mismatch lists each differing word: address, offset, expected and read value. Resuming won't fix a mismatch. If you think the link misread, run a fresh `--verify-only`.

To compare a pass file by hand: `isp-compare.py <dir>/isp/ldrom-pass1.txt <file.bin> --base 0x00100000`. Exit codes: 0 match, 1 differ, 3 not complete yet.

Then flash apps over USB:

```sh
python3 tools/host/gemifi.py ports
python3 tools/host/gemifi.py flash path/to/app.gfw
python3 tools/host/gemifi.py log
```

## 6. Recovery: why you can't brick it

- **SWD can't be locked out by firmware.** The ICE_DAT/ICE_CK pins are dedicated debug pins, not GPIO, so no code can turn them off. The only lock is CONFIG0's LOCK bit, which no gemifi script ever writes. Even a locked chip can be recovered with a chip erase, though that loses the firmware on it, which is why you keep backups.
- **If SWD won't attach** (code that stops clocks, sleeps or resets in a loop): hold **/RESET, U1 pin 46**, to GND while OpenOCD connects, then let go. Pin 46 is on the left edge, third from the bottom, with the dot at the bottom left. Pins 44/45 are the crystal and 47 is ground. Use a wire held by hand. Don't use the Bus Pirate's AUX pin: it drives only 3.3 V.
- **Stock firmware:** `restore-stock.sh <backup>` puts APROM and LDROM back (and data flash with `--with-dataflash`).
- **Bootloader broken?** `flash-ldrom.sh <backup>/ldrom.bin --i-have-a-backup <backup>` puts the stock LDROM back. APROM stays as it is.
- **Bus Pirate wedged?** See the plug order in §2.

## 7. Reference

### SWD scripts (`tools/swd/nuvoton-nuc1xx/`)

| Script | Writes? | Use |
|---|---|---|
| `check.sh` | no | Chip ID + `mdw 0x0 4` lock check |
| `read-config.sh` | no | CONFIG0/1 by ISP, decoded. `--decode 0xFF4FFF7F` / `--decode-file config.txt` work offline |
| `dump.sh <outdir> [--resume] [--compare <dir>]` | no | Full backup (see step 2) |
| `flash-aprom.sh <file.gfw\|file.bin> [--yes] [--no-reset]` | APROM | Write + verify + reset |
| `flash-aprom.sh --verify-only <file>` | no | Compare APROM with a file (`verify_image`) |
| `flash-ldrom.sh <boot.bin> --i-have-a-backup <dir> [--yes]` | LDROM | Guarded; ISP verify |
| `flash-ldrom.sh --verify-only <boot.bin> [--resume <dir>]` | no | ISP read-back + compare; resumable (§5) |
| `restore-stock.sh <backup> [--with-dataflash] [--write-config] [--yes]` | APROM, LDROM (+data flash, +CONFIG only on request) | Back to stock |
| `isp-dump.sh <dir> <name> <base> <size> <pass>` | no | Resumable ISP dump of one region (what `dump.sh` uses) |
| `isp-assemble.py <dir> <name> <base> <size> [--config] [--passes 1]` | – | Build a backup file from two identical passes |
| `isp-compare.py <pass.txt> <file> --base <addr> [--size N]` | – | Compare one pass with a known file; lists differing addresses and words not read yet |
| `read-only-check.sh` | no | Old name for `check.sh` |

All shell scripts above take `--probe buspirate|cmsis-dap` and `--dry-run`.

Offline tests (fake `openocd` on PATH, no hardware): `cd tools/swd/nuvoton-nuc1xx && python3 -m unittest`.

Environment variables:
- `PROBE=buspirate|cmsis-dap` (`--probe` overrides it);
- `BP_PORT=/dev/ttyUSB0`;
- `DRY_RUN=1`;
- `OPENOCD_TIMEOUT` (900 s), `OPENOCD_TIMEOUT_APROM` / `OPENOCD_TIMEOUT_WRITE` (1800 s);
- `LOG_DIR` (default `tools/swd/nuvoton-nuc1xx/logs/`, which is gitignored). Every session's output is logged there.

Supporting files:
- `openocd-buspirate.cfg`, `openocd-cmsis-dap.cfg`: the probe settings;
- `nuc1xx-target.cfg`: target and the four flash banks, with `-ap-num 0`;
- `lib.tcl`: PDID guard and ISP helpers;
- `isp-chunk.tcl`;
- `bincheck.py`, `backup-check.py`, `nuc1xx-config.py`: offline checks;
- `isp_dumpfile.py`: the pass-file reader shared by `isp-assemble.py` and `isp-compare.py`.

To use OpenOCD by hand: `openocd -s tools/swd/nuvoton-nuc1xx -f openocd-buspirate.cfg -f lib.tcl -c init -c gemifi_check_pdid …`

Safety built in:
- Every session that writes first reads the chip ID and aborts unless it's `0x10012008`.
- Each script prints its plan and asks for confirmation.
- Images are size- and vector-checked first.
- CONFIG is never written by default. OpenOCD 0.12 can't erase the NUC120 CONFIG bank with `flash erase_address`: the bank is 8 bytes, so it gets zero sectors. `--write-config` therefore uses ISP page-erase and program directly and reads the result back.

### Host tools (`tools/host/`, Python 3, standard library)

```sh
python3 tools/host/mkimage.py in.bin out.gfw --board 1 --version 0.1.0   # used by the firmware Makefiles
python3 tools/host/mkimage.py --check out.gfw [--board 1]
python3 tools/host/gemifi.py flash app.gfw | restore-stock FirmwareFile.hex.txt    # Code 61, see below
python3 tools/host/gemifi.py ports | hello | log | reboot app|boot [--wait 5]
python3 tools/host/gemifi.py flash app.gfw [--dry-run] [--port NAME] [--boot-port NAME]
python3 tools/host/gemifi.py flash app.bin --board 0x3F00 --version 1.2.3     # A/B device (ESP32/ESP8266)
python3 tools/host/gemifi.py --serial /dev/ttyUSB0 [--baud 115200] hello      # any command, over a serial port
python3 tools/host/gemifi.py schema | get | set | commit | factory-reset | backup | restore   # config, below
python3 tools/host/gemifi.py presets | preset select|save|reset|rename     # presets, below
cd tools/host && python3 -m unittest            # codec, mkimage, upload against a simulated bootloader, vectors
python3 tools/host/make_test_vectors.py         # regenerate protocol/test-vectors.json (--check to compare)
```

MIDI backends:
- `gemifi.py` uses `python-rtmidi` if it's installed, then `mido`, then raw ALSA (`/dev/snd/midiC*D*`, named from `/proc/asound`; no dependencies). Force one with `--backend`.
- The raw ALSA device can't be shared, so close other programs that have the port open.

How `gemifi.py flash` uploads (`protocol/sysex.md` §4.2):
1. Validates the header.
2. Reboots the app into the bootloader (a lost REBOOT reply counts as success), then waits for the `gemifi boot` port.
3. Sends `FW_BEGIN`.
4. Sends chunks from 0x200 upward, then page 0 last, never crossing a 512-byte page. Each request is stop-and-wait with a 200 ms timeout (2000 ms for `FW_BEGIN` and `FW_END`); a timeout, BAD_CHECKSUM or BUSY means the same seq is resent, up to 5 resends (6 sends in all).
5. Sends `FW_END`, then `REBOOT(app)`, and prints HELLO from the app.

On a device with presets it also backs up before step 2 and checks the presets after step 5 ("Presets and firmware updates" below).

It picks the path from HELLO: a device whose **app** answers with role 0, caps bit 0 (firmware upload) and a non-zero `max_chunk` takes the upload itself (A/B slots, below), and is never rebooted into a bootloader.

### ESP32 / ESP8266

ESP chips have no gemifi bootloader. The chip's ROM bootloader does the first install; after that the app updates itself over SysEx into its inactive OTA slot (`protocol/sysex.md` §4.1 "Who takes uploads", §4.2 "A/B devices").

**First flash (once, and for recovery)**: with esptool, or from the browser (`web/updater`, "Install firmware (first flash)", which runs esptool-js over Web Serial, like ESPHome and WLED). Give it a merged factory image written at 0x0 (`esptool.py merge_bin`, or PlatformIO's merged `.bin`), or an ESP Web Tools `manifest.json` with its part files:

```sh
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 factory.bin
```

If the board doesn't enter its bootloader by itself, hold BOOT (IO0) while pressing RESET.

**Serial transport**: a classic ESP32 or ESP8266 has no native USB, so a gemifi build for it (`GM_MIDI_SERIAL`) speaks raw MIDI bytes over the board's USB-UART at 115200 baud. Same protocol and frames; the tool cuts the SysEx out of the byte stream and drops everything else (the ROM's boot log, other MIDI). Every command takes `--serial DEV [--baud N]` (needs `pip install pyserial`; without it the tool says so). Opening the port releases RTS before DTR, so it doesn't reset the board through the usual auto-reset circuit. ESP32-S2/S3 builds with native USB show up as a USB-MIDI port like the Keystation and need no `--serial`.

```sh
gemifi.py --serial /dev/ttyUSB0 hello
gemifi.py --serial /dev/ttyUSB0 log --names
```

**Updates (OTA over SysEx)**: the image is the plain ESP app `.bin` (the build's `firmware.bin`, not the merged factory image), which has no `.gfw` header, so `--board` and `--version` are required. `--board` must equal the device's HELLO `board_id` (checked before anything is sent); `--version` is what HELLO must report afterwards.

```sh
gemifi.py flash .pio/build/esp32s3/firmware.bin --board 0x3F00 --version 1.2.3
gemifi.py --serial /dev/ttyUSB0 flash firmware.bin --board 0x3F00 --version 1.2.3
gemifi.py flash firmware.bin --board 0x3F00 --version 1.2.3 --dry-run --max-chunk 256
```

1. HELLO from the app: role 0 with the upload cap. Refuses a `.gfw` here, and a `.bin` on a bootloader device.
2. Pads the file with 0xFF to a multiple of 4. FW_BEGIN gets the padded size, the zlib CRC-32 over the padded file, `--board` and `--version`.
3. FW_DATA in plain order from offset 0, each chunk the largest multiple of 4 up to FW_BEGIN's `max_chunk`; no page rule. The old slot stays bootable throughout.
4. FW_END (the device checks the CRC and marks the new slot), then REBOOT(0), never REBOOT(1).
5. Waits for HELLO again (default 15 s; `--wait`) and checks that `fw_version` is the new one. If it isn't, the version given was wrong, or the new slot didn't boot and the device fell back to the old one.

### M-Audio Code 61 (STM32F1, board 0x0002)

The Code 61 keeps M-Audio's bootloader (0x08000000–0x080023FF). gemifi replaces only the app, through M-Audio's own SysEx update protocol, so the case stays closed. **None of this has run on hardware yet.** Background: `research/devices/m-audio-code61/analysis/update-protocol.md` and `custom-firmware-path.md`.

```sh
python3 tools/host/mkimage.py app.bin app.gfw --board 0x0002 --version 0.1.0 \
    --hdr-offset 0x130 --base 0x08002400 --max-size 102398        # what the platform Makefile runs
python3 tools/host/gemifi.py flash app.gfw [--dry-run]
python3 tools/host/gemifi.py restore-stock FirmwareFile_v1.2.1.hex.txt [--dry-run]
python3 tools/host/maudio_update.py info|build|syx|send|request ...   # the vendor protocol on its own
```

How `gemifi.py flash` does it:
1. Checks the `.gfw` (header at 0x130, linked at 0x08002400, at most 102,398 bytes). A raw `.bin` works too, with `--board 0x0002`.
2. Builds the update stream the way the stock file is laid out: one record per 1 KB page that holds the image (erase, load, program), an erase-only record for each page after it up to 0x0801AFFF, then page 0x0801B000 with the image checksum at 0x0801B3FE, written last. Built from the stock app, this gives the stock file byte for byte.
3. Asks the running app for update mode. A gemifi app gets HELLO, then REBOOT(1). The stock app gets the vendor start message and answers `F0 15 F7`.
4. Tells you to **power-cycle the keyboard**, and waits for the port to go away and come back (`--wait`, 120 s).
5. Probes the bootloader with the start message until it answers `F0 45 F7` (or `1A`). Then sends the start message again, and every record as ASCII-hex SysEx chunks, 3 ms apart, waiting up to 10 s for the ack after each record.
6. Waits for HELLO from the new gemifi app (`--after-wait`, 20 s) and checks the version.

If anything fails, the update request stays set. Power-cycle and run the same command again, or go back to stock with `restore-stock` and M-Audio's update file (never commit that file).

Options for the vendor protocol: `--boot-port NAME` (the bootloader's port name isn't known yet; the default looks for "Code" or "M-Audio"), `--ack-timeout`, `--probe-timeout`, `--gap` (ms), `--record-retries N` (default 0: the stock updater doesn't resend), `--no-restart` (no second start message after the probe).

Unknowns that a first hardware run has to settle: whether the bootloader takes a stream from our tool at all, what `45` and `1A` each mean, whether `0D`/`0E` reboot the keyboard by themselves, and whether a soft reset with the request flag set is enough (otherwise it's always a power cycle). Do the safe steps in `custom-firmware-path.md` §8 first: send the stock stream with `restore-stock` before any gemifi image.

### Config from the command line (`gemifi.py`, app firmware)

These talk to the app (not the bootloader) and need the `config` cap in HELLO. The tool reads the device's schema first (`protocol/sysex.md` §4.3–4.4), so nothing in it is board-specific.

```sh
gemifi.py schema                      # table of params: id, name, type, count, range, unit, default, label
gemifi.py schema --json               # parsed schema as JSON; --raw schema.bin saves the raw bytes
gemifi.py get                         # every value of every param, by group; enums by option label
gemifi.py get midi.channel 9:3 button.action:Sustain
gemifi.py set midi.transpose=-5 "button.action:Oct +=cc toggle" --commit
gemifi.py commit                      # store the live values in flash
gemifi.py factory-reset [--yes]       # defaults + erase the stored profile (asks first)
gemifi.py backup kms32.json           # {"board_id", "fw_version", "values": {name: value or [values]}}
gemifi.py restore kms32.json [--commit]
gemifi.py log --names                 # LOG/EVENT/PARAM_CHANGED, with param names from the schema
```

- A param is named by its `name` or `id`; array params take `:INDEX`, where INDEX is a number or an index label (any case; the component's display name, YAML `name:`). `set` needs the index for array params.
- Values: a number (`0x..` works), an enum option label (any case), `on`/`off` (also true/false, yes/no) for bool, and for note params a note name with **MIDI 60 = C4** (so 0 = C-1, 127 = G9; `F#3`, `Bb2` work). A trailing unit is allowed (`bend.glide=120ms`).
- `set` checks every value against the schema before sending anything. SET_PARAM is live at once; nothing is kept over a power cycle until `commit` (or `--commit`).
- `get`, `backup` and `restore` use GET_PARAMS/SET_PARAMS (runs of 16) for array params and fall back to single GET/SET_PARAM if the device replies BAD_CMD. If a run fails in `restore`, it retries that run one value at a time, so each bad entry is reported on its own.
- `backup` stores raw values by param name. `restore` matches by name: names the firmware doesn't know are skipped with a warning, read-only params are skipped, BAD_VALUE/BAD_PARAM is reported per entry and the rest still goes in (exit code 1 if any entry failed). A backup from another board gives a warning, not an error.
- COMMIT and FACTORY_RESET use the 2000 ms timeout.

### Presets (`gemifi.py`, app firmware with HELLO caps bit 5)

`protocol/sysex.md` §4.5. GET/SET always address the **active** preset, so `get`, `set` and `commit` above work on it.

```sh
gemifi.py presets                          # every slot: number, name, * = active, program, state
gemifi.py preset select 2                  # or by name: preset select drums
gemifi.py preset save                      # store the live values into the active preset (= commit)
gemifi.py preset save 5 --name "Live set"  # save as: into slot 5, which becomes the active one
gemifi.py preset reset 3 [--yes]           # slot 3 back to the firmware's preset (edits and name dropped)
gemifi.py preset reset all [--yes]
gemifi.py preset rename 2 "Kit"            # "" = back to the firmware's name
```

- `presets` shows per slot: "unsaved edits" (the active one only), "stored edits" (with the revision they were made on, when it isn't the firmware's), "empty slot" (not in the YAML).
- `preset select` asks first when the active preset has unsaved edits, because switching drops them; `--discard` doesn't ask.
- A preset is a number (1..count) or a name (any case).
- Names are 0..16 printable ASCII characters.

**Backup and restore with presets.**
- `backup` switches to every slot in turn and reads its per-preset values, then switches back to the slot that was active and puts its unsaved edits back, so the device ends as it started. The file (format 2) holds the globals in `values`, the active slot in `active`, and one entry per slot in `presets` (`slot`, `name`, `stored`, `stored_revision`, `values`). The active slot is saved as it is live, unsaved edits included.
- `restore` of such a file sets the globals, then for each slot: PRESET_RESET; if the backup has stored edits for it, select it, set its values, PRESET_SAVE and set its name. Then it selects the slot that was active and commits. So everything is stored; `--commit` isn't needed. Slots the device doesn't have are skipped with a warning.
- An old backup (no presets) restores into the active preset, live, as before. A preset backup onto firmware without presets restores the globals and the active slot only.

**Presets and firmware updates (`flash`).**
- Before the upload, on a device with presets, `flash` saves a backup to `gemifi-backup-<board>-<version>-<time>.json` in the current directory. `--backup FILE` picks the name (and also works on a device without presets); `--no-backup` skips it. If the device is already in the bootloader, there is nothing to read, and it says so.
- After the upload, it reads PRESET_INFO for every slot. If a slot's stored edits were made on another presets revision (the firmware's YAML presets changed), it lists them and asks: keep your edits on top of the new presets (Enter, the default), or use the firmware's presets (PRESET_RESET on every slot; names too; the globals stay). `--keep-presets` and `--use-firmware-presets` answer in advance. Either way, `gemifi.py restore <backup>` undoes it.
