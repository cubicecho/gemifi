# gemifi tools: first-steps runbook

This is the order to do things in when you first put your own code on the **M-Audio Keystation Mini 32 MK3** (Nuvoton NUC120LC1BN). Each step can be undone.

| Folder | What's in it |
|---|---|
| `swd/nuvoton-nuc1xx/` | SWD scripts (OpenOCD): check the chip, back it up, flash APROM/LDROM, restore stock |
| `host/` | `mkimage.py` (makes `.gfw` images), `gemifi.py` (USB MIDI: hello, log, reboot, flash), tests |
| `usb/probe-midi-device.sh` | read-only USB/MIDI probe of the stock keyboard (lsusb, identity request, capture) |

All scripts work from any directory. Every script that writes to flash prints what it will do and waits for you to type `yes`, unless you pass `--yes`. **No script writes CONFIG by default.** Only `restore-stock.sh --write-config` does, and it asks you to type `write config` first.

Add `--dry-run` (or set `DRY_RUN=1`) to any SWD script to print the exact `openocd` commands without touching the probe.

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

**Raspberry Pi Pico (debugprobe), when you have it:** GP2 (SWCLK) → TP2, GP3 (SWDIO) → TP1, GND → TP4; nothing else. Then run every script with `PROBE=cmsis-dap`. It's roughly 100× faster than the Bus Pirate.

## 2. Plug order (important)

The keyboard leaks 5 V into the Bus Pirate through MOSI/CLK (back-powering). Once that happens, unplugging and replugging the Bus Pirate no longer resets it.

- **Plugging in:** plug in the **Bus Pirate first**, then the **keyboard last**.
- **Unplugging:** unplug the **keyboard first**, then the Bus Pirate.
- **If the Bus Pirate stops answering** (an OpenOCD session was killed, timed out or hung), it's stuck in binary mode. Unplug the keyboard, then the Bus Pirate. Plug in the Bus Pirate, then the keyboard. Rerun the command. Dumps pick up where they stopped.
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

**It never writes CONFIG** unless you pass `--write-config`. Even then it only writes when CONFIG differs: it shows the old and new values decoded and waits for you to type `write config`. It refuses any value that would lock the chip.

Unplug the keyboard (first), plug it back in, and check that it works as a normal MIDI keyboard again (`tools/usb/probe-midi-device.sh`).

Repeat steps 3 and 4 as often as you like. That's the M1/M2 development loop. M3 (bootloader test build in APROM, `bootloader/…/build/test-aprom/gemifi-boot-test.bin`) goes in the same way through `flash-aprom.sh`.

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
| `flash-ldrom.sh <boot.bin> --i-have-a-backup <dir> [--yes]` | LDROM | Guarded; ISP verify |
| `restore-stock.sh <backup> [--with-dataflash] [--write-config] [--yes]` | APROM, LDROM (+data flash, +CONFIG only on request) | Back to stock |
| `isp-dump.sh <dir> <name> <base> <size> <pass>` | no | Resumable ISP dump of one region (what `dump.sh` uses) |
| `isp-assemble.py <dir> <name> <base> <size> [--config] [--passes 1]` | – | Assemble an ISP dump |
| `read-only-check.sh` | no | Old name for `check.sh` |

Environment variables:
- `PROBE=buspirate|cmsis-dap`;
- `BP_PORT=/dev/ttyUSB0`;
- `DRY_RUN=1`;
- `OPENOCD_TIMEOUT` (900 s), `OPENOCD_TIMEOUT_APROM` / `OPENOCD_TIMEOUT_WRITE` (1800 s);
- `LOG_DIR` (default `tools/swd/nuvoton-nuc1xx/logs/`, which is gitignored). Every session's output is logged there.

Supporting files:
- `openocd-buspirate.cfg`, `openocd-cmsis-dap.cfg`: the probe settings;
- `nuc1xx-target.cfg`: target and the four flash banks, with `-ap-num 0`;
- `lib.tcl`: PDID guard and ISP helpers;
- `isp-chunk.tcl`;
- `bincheck.py`, `backup-check.py`, `nuc1xx-config.py`: offline checks.

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
python3 tools/host/gemifi.py ports | hello | log | reboot app|boot [--wait 5]
python3 tools/host/gemifi.py flash app.gfw [--dry-run] [--port NAME] [--boot-port NAME]
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
