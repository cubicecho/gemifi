# gemifi bootloader: Nuvoton NUC100/NUC120 (LDROM)

This bootloader runs from the 4 KB LDROM of a NUC1xx part. It hands off to the app in APROM. When asked, it takes a new app over USB-MIDI SysEx ([`protocol/sysex.md`](../../protocol/sysex.md)).

First board: M-Audio Keystation Mini 32 MK3 (NUC120LC1BN). Its board file is [`platform/nuvoton-nuc1xx/boards/keystation-mini32-mk3.h`](../../platform/nuvoton-nuc1xx/boards/keystation-mini32-mk3.h).

> **Status: built and checked on the host only. Nothing here has run on hardware yet.** Test it in APROM first (`TEST_IN_APROM=1`, below) before putting it in LDROM.

## Build

```sh
make                       # build/gemifi-boot.bin: for LDROM (programmed at 0x0010_0000)
make TEST_IN_APROM=1       # build/test-aprom/gemifi-boot-test.bin: for APROM (0x0), test variant
make clean
```

**Toolchain.** The build uses `arm-none-eabi-gcc` from `PATH` (Ubuntu: `gcc-arm-none-eabi`). To use another one, for example an Arm GNU Toolchain tarball, pass its prefix:

```sh
make CROSS=/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-
```

**Flags.** `-mcpu=cortex-m0 -mthumb -Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections -nostartfiles -nostdlib` (plus libgcc). The rules are in [`platform/nuvoton-nuc1xx/mk/rules.mk`](../../platform/nuvoton-nuc1xx/mk/rules.mk).

**Size.** The link fails if the image goes over 4096 bytes (`ASSERT` in `ld/bootloader.ld`). Sizes with arm-none-eabi-gcc 14.2.1 (Ubuntu 14.2.rel1):

| Build | Size | Headroom |
|---|---|---|
| `gemifi-boot.bin` | 4044 B | 52 B |
| `gemifi-boot-test.bin` | 3996 B | 100 B |

Headroom is tight. Before adding anything, see "Size notes" below.

## Memory map

| Region | Address | Use |
|---|---|---|
| LDROM | 0x0010_0000, mapped at **0x0** while it runs (CONFIG0 CBS = 0) | this bootloader, linked at 0x0 |
| APROM | 0x0000_0000, 32 KB | the app: vectors 0x000–0x0BF, `.gfw` header 0x0C0–0x0DF |
| Data flash | 0x0001_F000, 4 KB | owned by the app; this bootloader never writes it (only the test build does) |
| CONFIG0/1 | 0x0030_0000 | never written; keep CBS = 0 so LDROM always runs first |
| SRAM | 0x2000_0000 – 0x2000_0FEF | `.bss` (~880 B) and the stack; the stack starts at 0x2000_0FF0 |
| Mailbox | 0x2000_0FF0 – 0x2000_0FFF | `magic`, `magic_inv`, `boot_count`, `reason`; not initialised by either side |

## Boot flow

1. Set up the clocks: crystal, then the PLL at 48 MHz. This is safe even if an earlier stage left the PLL running.
2. Set up the GPIOs (buttons as inputs, LEDs off, matrix drive lines low) and enable ISP.
3. Decide whether to stay in update mode. It stays if **any** of these is true:
   - the mailbox holds `'BOOT'` (0x544F4F42) with a matching inverse; the app's `REBOOT(1)` sets this;
   - **Pitch▼ + Pitch▲** (PA5 + PA6) are held, sampled 3 times over about 20 ms;
   - the app is invalid. Checks: magic, `hdr_version`, `board_id`, size, initial SP in SRAM, reset vector odd and inside the image, and the CRC-32 over `[0, size)` with the crc field taken as 0.
4. The mailbox magic is cleared either way.
5. Otherwise it hands off: `boot_count++`, unlock REGWRPROT, `ISPCON.BS = 0`, `IPRSTC1.CPU_RST`. USB was never started on this path.

## Update mode

- **USB identity.** `1209:0001`, "gemifi" / "gemifi boot (Keystation Mini 32)", serial = chip UID in hex. It is a class-compliant USB-MIDI device with one port: bulk 0x01 OUT and 0x81 IN, 64 B each, and EP0 at 64 B.
- **LEDs.** While waiting, the Oct− and Oct+ LEDs blink alternately at 2 Hz. During an upload, between `FW_BEGIN` and `FW_END`/`FW_ABORT`, both blink together at 5 Hz.
- **Commands.**
  - `HELLO` replies with role 1, `max_chunk` 256 and caps = upload.
  - `REBOOT(0|1)`: after about 50 ms the bootloader drives SE0 for about 10 ms, then does `ISPCON.BS = 1` + CPU reset. Mode 1 also sets the mailbox magic. Both modes go back through the bootloader, which starts the app only if it is valid.
  - `FW_BEGIN / FW_DATA / FW_END / FW_ABORT` follow sysex.md §4.2:
    - `FW_BEGIN` erases page 0 first.
    - Data must arrive in order from 0x200 up, with page 0 last.
    - Each page is erased when its first chunk arrives.
    - Every word is read back after programming.
    - `FW_END` re-checks the whole image, and its size, CRC and `fw_version` must match `FW_BEGIN`.
  - A request repeated with the same `seq` gets the cached reply and the work is not redone. BAD_CHECKSUM replies don't count as completing a request.
- **Address safety.**
  - The protocol accepts only offsets inside `[0, size)` with `size` ≤ 32 KB.
  - `target_addr()` in `main.c` then re-checks every erase and program address against `[TARGET_BASE, TARGET_BASE + TARGET_SIZE)`.
  - LDUEN and CFGUEN are never set (they are masked out of every ISPCON write), so the FMC itself refuses LDROM and CONFIG writes.

## Test variant: `TEST_IN_APROM=1`

Milestone M3 in the design doc. The same code is linked for **APROM** and flashed at 0x0, with the stock LDROM left in place. The stock LDROM hands off to APROM unconditionally, so the test bootloader runs on every power-up. It differs from the real build in three ways:

- It **always** stays in update mode, because there is no app to start.
- Uploads go to **data flash** (0x1F000, 4 KB) as a stand-in for APROM. APROM can't program itself, and LDROM stays untouched. Images up to 4 KB can be uploaded: `hello-led.gfw` (652 B) fits, `usb-log.gfw` (4112 B) doesn't. `FW_END` runs the full image check on the data-flash copy.
- The USB product string is "gemifi boot test (Keystation Mini 32)".

`REBOOT` resets straight back into APROM. **This overwrites the stock settings journal in data flash.** The backup in `research/.../backup/dataflash.bin` restores it (`restore-stock.sh --with-dataflash`).

## Flashing over SWD

Use the scripts in [`tools/swd/nuvoton-nuc1xx/`](../../tools/swd/nuvoton-nuc1xx/). They wrap OpenOCD with backups, checks and verification. Take a fresh `dump.sh` backup first.

```sh
# M3: test variant in APROM (stock LDROM and CONFIG untouched)
tools/swd/nuvoton-nuc1xx/flash-aprom.sh bootloader/nuvoton-nuc1xx/build/test-aprom/gemifi-boot-test.bin

# M4: the real bootloader into LDROM (needs a verified backup folder)
tools/swd/nuvoton-nuc1xx/flash-ldrom.sh bootloader/nuvoton-nuc1xx/build/gemifi-boot.bin --i-have-a-backup <dump-dir>

# Back to stock
tools/swd/nuvoton-nuc1xx/restore-stock.sh <dump-dir>
```

CONFIG is never written; CBS = 0 (boot from LDROM) must stay as it is. With the gemifi bootloader in LDROM and no valid app, the board powers up in update mode, so the next step is to upload an app over USB.

## Size notes

- The vector table holds only SP, Reset, NMI and HardFault. The bootloader is polled: no IRQ is ever enabled, and SVC, PendSV and the SysTick interrupt are never used.
- Registers are accessed as base + offset (`RB()` in `nuc1xx.h`), so GCC shares one literal per peripheral block. Absolute addresses cost about 300 B more.
- `.data` is empty, and `mini_libc.c` is not linked. If GCC ever emits `memcpy`/`memset`, the link fails loudly.
- The SysTick millisecond counter is polled (COUNTFLAG). Time spent stalled in a flash erase is not counted, which only makes timeouts longer.

## Files

| File | What |
|---|---|
| `main.c` | boot decision, image check, SysEx request handling, upload state machine |
| `Makefile` | build, plus the `TEST_IN_APROM` switch |
| `../../platform/nuvoton-nuc1xx/` | startup, linker scripts, clocks, USB device core, USB-MIDI, FMC, CRC-32, SysEx codec, board files |
