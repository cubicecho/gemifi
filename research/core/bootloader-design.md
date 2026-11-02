# Keystation Mini 32 MK3: bootloader and USB "base" options

2026-10-30. Research for roadmap steps 1–4 in `../NOTES.md`. The goal is firmware upload over USB from the first step, because the app code will change constantly. We want a minimal bootloader that starts the main firmware normally and takes updates from a browser page and a command-line tool. The running app also needs a debug log.

Inputs: `../NOTES.md`, `../probe-findings.md`, `ldrom.md` (stock LDROM), `hw-map.md` (pins), the NUC100/NUC120 TRM V2.02 and datasheet, Nuvoton's NUC100 BSP, TinyUSB, OpenDeck and the OpenOCD sources. Sources are listed at the end. **(unverified)** marks a claim I couldn't check against a primary source or on hardware.

---

## TL;DR

- **Recommendation: a custom bare-metal USB-MIDI bootloader in LDROM that takes firmware as SysEx (option B).**
  - **Size:** about 2–2.5 KB of the 4 KB LDROM.
  - **One protocol, one page:** the app uses the same SysEx protocol for config, logs and "reboot to bootloader". So one Web MIDI page (and one Python tool) handles config, logging and flashing.
  - **Drivers and browsers:** no driver installs on Windows, macOS or Linux, and no udev rules on Linux. Web MIDI works in Chrome, Edge and Firefox. Safari has no Web MIDI, Web Serial, WebUSB or WebHID, so no option helps there.
  - **Proven on this board:** the stock M-Audio/Medeli LDROM is itself a USB-MIDI SysEx updater that fits in 4 KB (`ldrom.md`).
- **Keep CONFIG0 as it is (CBS=0, boot from LDROM).** Our LDROM then runs first on every power-up, which is what makes the design hard to brick:
  1. It checks for a button combo (**Pitch▼ + Pitch▲ held at plug-in**) and a "stay in bootloader" magic word left in SRAM by the app.
  2. It checks the app's CRC32.
  3. Then it hands off the same way the stock LDROM does: `ISPCON.BS=0`, then `IPRSTC1.CPU_RST`.
  - We never have to write CONFIG at all.
- **Three hardware facts decide most of this:**
  1. **USB endpoint budget.** The USBD has 6 simplex endpoint slots and control uses 2, so 4 are left. MIDI needs 2 and CDC-ACM needs 3, so **MIDI + CDC does not fit in the app or the bootloader**, at any code size. Linux `cdc-acm` refuses a CDC interface with no notification endpoint.
  2. **APROM can't program itself** on this silicon (TRM V2.02: ISPFF is set when "APROM writes to itself", and there is no APUEN bit). This rules out an APROM-resident bootloader (option D).
  3. **LDROM can't program itself either.** So a USB bootloader in LDROM can never overwrite itself. That protection is in hardware, not in our code.
- **Debug log: SysEx text/event messages on the MIDI port.** This costs no endpoints and shows up in the same web page and the Python tool. There's no free UART (`hw-map.md`): the UART pins are matrix sense lines. If a real serial terminal is ever needed, there are two fallbacks:
  - a compile-time "debug personality" (CDC-ACM + MIDI IN only = 4 slots);
  - bit-banged TX on PB.15 (pin 43), if it reaches a pad.
- **First milestone: flash only APROM over SWD and keep the stock LDROM and CONFIG.** The stock LDROM already hands off to APROM unconditionally, so we can prove clocks, USB-MIDI, SysEx logging and the "reboot to bootloader" path before we touch LDROM.

---

## 1. Facts that constrain the design

| Fact | Source | Consequence |
|---|---|---|
| USBD has **6 endpoint slots**, each configured as IN *or* OUT with its own `EP_NUM` (`USB_CFGx`: `STATE` 01 = OUT, 10 = IN, `EP_NUM[3:0]`). They share 512 B of USB SRAM. | TRM V2.02 §5.4 (USB_CFGx); TinyUSB `dcd_nuc120.c` header comment: "six EPs, but each is simplex, so two … are needed to implement USB EP0 … This leaves up to four for user usage." | **4 data pipes for the application.** The same endpoint *number* can be used for IN and OUT (0x01/0x81), but each direction still takes a slot. MIDI = 2, CDC-ACM = 3 (notify IN + bulk IN + bulk OUT), vendor/WebUSB = 1–2, HID = 1–2, DFU = 0 (EP0 only). |
| ISPCON bit 3 is **reserved** in TRM V2.02 (no APUEN). ISPFF is set if "(1) APROM writes to itself, (2) LDROM writes to itself". LDUEN: "LDROM can be updated when the chip runs in APROM". | TRM V2.02 §6 FMC ISPCON | Code running in APROM can't erase or program APROM, so there is **no APROM bootloader and no in-app self-update**. LDROM code can write APROM and data flash. Code in LDROM can't overwrite LDROM. The newer BSP has `FMC_EnableAPUpdate()` (APUEN) for later NUC100 revisions; whether our NUC120LC1BN has it is **(unverified)**. |
| `BS` "is initiated with the inverted value of CBS after power-on reset; it keeps the same value at other reset". CHIP_RST "is the same as POR … chip setting from flash are also reloaded". CPU_RST resets only the CPU and FMC. | BSP `NUC100Series.h` register docs; TRM | Hand off with `BS` + **CPU_RST** (or SYSRESETREQ), never CHIP_RST. The stock LDROM does exactly this (`ldrom.md` §2). CPU_RST doesn't reset the USB block, so drive SE0 / reset USBD before switching so the host re-enumerates. |
| When CBS=0, LDROM is mapped at 0x0. Cortex-M0 has **no VTOR**. | `ldrom.md` (stock LDROM vectors at 0, reset 0xF65); ARMv6-M | The bootloader is linked at **0x0** (4 KB) and programmed at 0x00100000. The app is linked at **0x0** and starts from a real reset, so it needs no vector relocation. |
| Flash: 512 B pages, page erase 20–40 ms, endurance **10,000 cycles**. The CPU stalls during ISP and interrupts wait. | Datasheet §8 flash specs; TRM §6.8 | Erasing 32 KB of APROM takes about 1.1–2.2 s. USB NAKs during the stall, which is harmless. Config in data flash needs a journal (the stock firmware already uses one). |
| 12 MHz crystal. PLLCON=0xC22E gives 48 MHz (HCLK and USB). | `ldrom.md`, `hw-map.md` | The bootloader copies this clock setup. |
| Buttons are direct GPIOs PA1..PA7 (Sustain, Edit, Oct−, Oct+, Pitch▼, Pitch▲, Mod), active-low with external pull-ups. LEDs PA10–PA15 are active-low. There is **no free UART**. PB.15 (pin 43) is the only unused GPIO. | `hw-map.md` §2–3 | Entry combo detection is a single read of `PA_PIN`, with no matrix driving and no risk of shorting lines. The octave LEDs can show bootloader status. |
| The stock app treats **Oct− + Oct+ at power-up as factory reset**, and **Sustain + Edit + Oct−** makes `main()` exit. The stock LDROM enters its updater on a key-matrix condition (PB0/PB1 rows). | `hw-map.md` §3, `ldrom.md` §1 | Pick a combo that clashes with neither: **Pitch▼ + Pitch▲ (PA5 + PA6)**. |

---

## 2. Comparison of approaches

The table ranks the bootloader options. "Page" means the gemifi web page that also edits config over Web MIDI.

| | **A. Nuvoton ISP_HID** | **B. Custom USB-MIDI SysEx** ⭐ | **B′. Custom CDC-ACM (Web Serial)** | **C. USB DFU 1.1** | **D. APROM bootloader (TinyUSB)** | **E. UF2 mass storage** |
|---|---|---|---|---|---|---|
| Fits 4 KB LDROM? | Yes. Nuvoton targets LDROM (Keil, `NUC100_LD_4.FLM`) | **Yes** (the stock Medeli LDROM is the same kind of thing and uses 0xFF0 B) | Yes | Yes (Nuvoton ships `ISP_DFU` for LDROM. A bare-metal DFU on SAMD is 1 KB) | n/a: lives in APROM. **Not possible**: APROM can't write itself | **No** (8 KB on SAMD; tinyuf2 is larger) |
| Est. size (gcc -Os, bare metal) | ~3–3.8 KB as shipped (BSP USBD driver); unknown under GCC | **~2.0–2.5 KB** | ~1.8–2.3 KB | ~1.2–2 KB (+~0.2 KB with MS OS 2.0 descriptors for WinUSB) | TinyUSB core + MIDI ≈ 6–9 KB of the 32 KB | > 6 KB |
| Browser API | WebHID | **Web MIDI** | Web Serial | WebUSB (webdfu) | Web MIDI | none (drag and drop in a file manager) |
| Browsers | Chrome/Edge/Opera | **Chrome/Edge/Opera + Firefox** (site-permission add-on prompt) | Chrome/Edge/Opera + Firefox ≥ 151 | Chrome/Edge/Opera | Chrome/Edge + Firefox | any OS file manager |
| Drivers: Win / mac / Linux | none / none / **udev rule** (hidraw is root-only by default) | **none / none / none** | none (usbser) / none / `dialout` group; **ModemManager may probe the port** | **WinUSB needed** (MS OS 2.0 descriptors make it automatic) / none / **udev rule** | none | none |
| Same page as the Web MIDI editor? | Same page, second API and second permission. No Firefox | **Yes, same API, protocol and permission** | Same page, second API. App can't expose CDC alongside MIDI (endpoints) | Same page, second API. Chromium only | Yes | No |
| Desktop tool | NuMicro ISP Tool (Windows) | small Python script (rtmidi, or raw ALSA `/dev/snd/midiC*`) | pyserial / any terminal | **dfu-util** | Python | cp |
| Effort | Medium: port Keil → GCC, replace PD0 entry, add CRC check, remove CONFIG/erase-all commands | Medium: ~600 lines of C (USB core + MIDI + protocol) | Medium, slightly less than B | Medium-low (simple spec; good references) | Low for USB, but blocked by hardware | High |
| Brick risk | Medium as shipped: `CMD_UPDATE_CONFIG` and `CMD_ERASE_ALL` can rewrite CONFIG over USB; no app check | **Low** (LDROM is hardware-protected; CRC gate; CBS=0 means LDROM always runs first) | Low | Low | High (bootloader in the region it would erase) | n/a |

**Why B over B′ (CDC) when only one class fits in the bootloader:**
- The app has to speak MIDI anyway. A MIDI bootloader means one transport, one parser and one browser permission, end to end.
- With B′, the page would use Web MIDI for the app and Web Serial for the bootloader: two permission prompts, two code paths. The app also can't offer CDC next to full MIDI (§5).
- Web MIDI needs nothing installed on Linux. CDC needs the `dialout` group, and ModemManager may probe a new `ttyACM` with AT commands while the bootloader is talking (**(unverified)** on current Ubuntu; it can be stopped with a udev `ID_MM_DEVICE_IGNORE` rule).
- What B costs: 8→7-bit packing (about 15 lines of C, 14% overhead) and USB-MIDI event packing (CIN 4/5/6/7). Both are trivial. Raw binary over CDC is only marginally simpler.

**MIDI + CDC composite in the 4 KB bootloader?** Code size isn't the problem: about 2.6–3.2 KB would fit. **The endpoint budget is**: 2 (control) + 2 (MIDI) + 3 (CDC) = 7 > 6 slots. A composite without the CDC notification endpoint is rejected by Linux: `cdc-acm.c` `acm_probe()` returns `-EINVAL` when `control_interface…bNumEndpoints == 0`. Windows `usbser.sys` and macOS behaviour without it is **(unverified)**, so don't rely on it.

### Notes per option

**A. Nuvoton ISP_HID** (`NUC100BSP/SampleCode/ISP/ISP_HID`, Apache-2.0):
- **USB identity:** VID 0x0416, PID 0x3F00. 64-byte HID reports on EP2 (IN) and EP3 (OUT), 8-byte EP0. Polled; no USB interrupt.
- **Request packet:** `[cmd u32 LE][packno u32][56 B payload]`.
- **Response packet:** `[u16 byte-sum of the request][packno+1 u32][data]`.
- **Commands:**
  - firmware: `A0` UPDATE_APROM (erases all of APROM, then streams; the start address is ignored), `C3` UPDATE_DATAFLASH, `A3` ERASE_ALL (also sets the CONFIG lock bit!)
  - config words: `A1` UPDATE_CONFIG, `A2` READ_CONFIG
  - session: `A4` SYNC_PACKNO, `AE` CONNECT, `FF` RESEND_PACKET
  - info: `A6` GET_FWVER, `B1` GET_DEVICEID
  - reboot: `AB` RUN_APROM, `AC` RUN_LDROM, `AD` RESET
- **Weaknesses:**
  - Entry is "PD0 low" only.
  - There's no app validity check.
  - The report descriptor uses Generic Desktop usage 0, which Chrome's WebHID may block as a protected collection (**(unverified)**; vendor page 0xFF00 would be safer).
  - Only Keil projects ship; there's no GCC build.
- Useful as a reference for the FMC code. Not worth adopting as-is.

**C. DFU:**
- **Nuvoton's sample:** `ISP_DFU` (VID 0416, PID BDF0, DFU 1.1, `wTransferSize` 1024, DfuSe-style `0x21` set-address and `0x41` erase) ships with a libusb/libusbK Windows driver package. Shipping one tells you a driver is expected on Windows.
- **Browser:** webdfu (ISC licence) works from Chromium via WebUSB.
- **Drivers:** Windows needs WinUSB bound (MS OS 2.0 descriptors can do that automatically). Linux needs a udev rule for `/dev/bus/usb`.
- **Strong point:** the app can expose a *DFU runtime* interface (detach) with **zero endpoints**, and dfu-util is a standard tool.
- **Verdict:** a good secondary option if we ever want a non-MIDI recovery path. It loses on Firefox and on Linux/Windows setup.

**D. APROM bootloader with TinyUSB:**
- TinyUSB fits easily in APROM (est. 6–9 KB for core + MIDI with -Os/LTO; TI's TinyUSB guide reports 10–15 KB for CDC/HID examples at -O2 *including* their driverlib, which brackets it).
- But per TRM V2.02, APROM can't erase or program APROM, and Cortex-M0 has no VTOR, so an app at an offset would need a vector-forwarding shim.
- **Not viable** unless the APUEN bit turns out to exist on this die. That's worth a quick read-only test of `ISPCON` bit 3 later, but don't design around it.

**E. UF2:**
- Microsoft's `uf2-samdx1` reserves an 8 KB bootloader region. A UF2 bootloader needs FAT12 emulation, SCSI and MSC Bulk-Only Transport.
- Not realistic in 4 KB, and it has no browser path.

---

## 3. Recommended design

### 3.1 Flash layout

| Region | Address | Size | Owner | Written by |
|---|---|---|---|---|
| LDROM | 0x0010_0000 (mapped at 0x0 while running) | 4 KB | **gemifi bootloader** | SWD only. LDROM can't write itself; the app never sets LDUEN |
| APROM | 0x0000_0000 | **32 KB, all of it for the app** (vectors 0x000–0x0BF, image header at 0x0C0) | app | bootloader (USB), or SWD |
| Data flash | 0x0001_F000 | 4 KB = 8 × 512 B pages | app config (profile journal, two-page A/B or append-journal like stock) | app only. The bootloader doesn't touch it except an explicit "factory reset" command |
| CONFIG0/1 | 0x0030_0000 | 8 B | **left as is**: 0xFF4FFF7F / 0xFFFFFFFF (CBS=0, boot LDROM) | never written over USB |

SRAM (4 KB): keep `0x2000_0FF0–0x2000_0FFF` as a shared **no-init mailbox** (magic, boot reason, boot counter). Both the bootloader and the app set their stack top to 0x2000_0FF0. Nuvoton's own FMC_IAP sample uses 0x20000FFC for a signature word in the same way.

### 3.2 Image format

The app is linked at 0x0. A post-build step (`tools/mkimage.py`) fills a 32-byte header at fixed offset **0x0C0**, just after the 48-entry vector table (16 core + 32 IRQs):

```c
struct gemifi_img_hdr {      // at 0x000000C0
  uint32_t magic;            // 'GMFI' 0x49464D47
  uint16_t hdr_version;      // 1
  uint16_t board_id;         // e.g. 0x0001 = Keystation Mini 32 MK3 / NUC120LC1BN
  uint32_t image_size;       // bytes from 0x0, multiple of 4, <= 0x8000
  uint32_t crc32;            // CRC-32 (IEEE, reflected) over [0, image_size) with this field taken as 0
  uint32_t fw_version;       // semver packed 8.8.16
  uint32_t proto_version;    // SysEx protocol version the app speaks
  uint32_t reserved[2];      // 0xFFFFFFFF
};
```

**App is valid if** all of these hold:
- the magic matches;
- `board_id` matches the bootloader's own;
- `image_size` is ≤ 32 KB;
- the initial SP is in 0x2000_0000..0x2000_1000;
- the reset vector is odd and lies inside the image;
- the CRC matches.

The bootloader reads APROM through ISP READ (APROM isn't at 0x0 while LDROM is mapped). A nibble-table CRC (64-byte table) over 28 KB takes roughly 5–10 ms at 48 MHz (est.).

### 3.3 Boot flow (LDROM)

```
reset (POR, or CPU_RST from the app)
 ├─ clocks: HXT 12 MHz → PLL 48 MHz (PLLCON 0xC22E), like stock
 ├─ read SYS->RSTSRC, mailbox; GPIO: PA1..PA7 inputs, LEDs PA10..15 high (off)
 ├─ stay = mailbox.magic == 'BOOT'            (app asked: "reboot to bootloader")
 │       || (PA_PIN & (PA5|PA6)) == 0, sampled 3× over ~20 ms   (Pitch▼ + Pitch▲ held)
 │       || !app_valid()                      (no app / bad CRC / wrong board)
 │       || mailbox.boot_count >= 3           (app keeps crashing, §3.6; zeroed on POR/LVR/BOD)
 ├─ clear mailbox.magic
 ├─ if !stay: mailbox.boot_count++; USB stays off; unlock; ISPCON.BS=0; IPRSTC1.CPU_RST=1   → app
 └─ else: bootloader mode: Oct LEDs blink; enumerate as USB-MIDI "gemifi boot"; serve SysEx
```

App side:
- **Startup:**
  - Don't assume reset-state clocks: the PLL may already be on and REGWRPROT may be unlocked (`ldrom.md` §2). Re-init both.
  - Pulse IPRSTC2.USBD_RST and DRVSE0 before enabling USB. The stock app does this (`hw-map.md` §4).
- **"Reboot to bootloader" (a SysEx command):**
  1. ACK the command.
  2. Wait about 50 ms.
  3. Set mailbox.magic = 'BOOT'.
  4. Disconnect (DRVSE0 for about 10 ms).
  5. Unlock, set `ISPCON.BS=1`, then `IPRSTC1.CPU_RST=1`. SYSRESETREQ also works; Nuvoton's ISP samples use it.
- **Optional hardening:** the app sets `ISPCON.BS=1` early in startup. Then *any* later non-POR reset (watchdog, a fault handler that resets) goes back through the LDROM, which can count boots (§3.6).

**Recovery story:**
- CBS=0 means LDROM gets control first on every power-up, so no app, however broken, can stop the combo check.
- A broken bootloader is fixed over SWD; the dedicated ICE pins can't be taken over by firmware.
- Connect-under-reset needs /RESET (pin 46), per `NOTES.md`.

### 3.4 USB identity

| | Bootloader | App |
|---|---|---|
| VID:PID | 1209:xxxx (pid.codes, free for open-source hardware). Use a pid.codes test PID while developing | 1209:yyyy, **a different PID** |
| Strings | "gemifi" / "gemifi boot (Keystation Mini 32)" / serial = chip UID (FMC UID read) | "gemifi" / "gemifi (Keystation Mini 32)" / same serial |
| Interfaces | Audio Control + MIDI Streaming (1 embedded IN jack, 1 embedded OUT jack), bulk EP 0x81 IN / 0x01 OUT, 64 B | same MIDI function (+ optional extras, §5) |
| EP0 | 64 B (stock uses 8; 64 is faster for descriptors) | 64 B |

Why separate PIDs:
- Windows (and some DAWs) cache port names per VID/PID/instance. Separate PIDs keep "boot" and "app" visibly different and stop a DAW from grabbing the bootloader port.
- **Don't reuse M-Audio's 0763:3111 or Medeli's 0A67:138F.**

### 3.5 Firmware upload over SysEx

These are the bootloader commands of the shared protocol (§6):
1. `FW_BEGIN(size, crc32, board_id, fw_version)`: checks size and board, **erases page 0 first** (the app is now invalid), and replies with the max chunk size.
2. `FW_DATA(offset, 256 B packed)`: offsets must be sequential. A page is erased when its first chunk arrives, then programmed word by word. Each chunk is ACKed (stop-and-wait).
3. `FW_END`: reads back and CRC-checks the whole image, then replies OK or BAD_CRC.
4. `REBOOT(app)`.

The host sends the image **from offset 0x200 upward and page 0 last** (the bootloader allows that one out-of-order page). The vectors and header only appear once everything else is written.

**Power loss at any point** leaves either an erased page 0 or a CRC mismatch, so the next power-up stays in the bootloader, and you retry.

**Est. throughput:**
- One chunk is a 256 B payload, which packs to ~293 SysEx bytes, ~98 USB-MIDI events and 7 USB packets.
- 28 KB is 112 chunks at ~5–15 ms round trip each (browser and OS latency dominate), so about 1–2 s.
- Erase adds 1.1–2.2 s. **Total ≈ 3–5 s.**

The bootloader **rejects** any write outside 0x0000–0x7FFF:
- LDROM and CONFIG can't be written anyway (no LDUEN/CFGUEN, and LDROM can't write itself);
- data flash stays app-owned.

### 3.6 Safety checklist

- The bootloader is never writable from USB. Hardware enforces this (LDROM self-write fails; LDUEN is never set by our code), and so does a software address check.
- No USB path writes CONFIG (unlike Nuvoton's ISP_HID).
- CRC32 is checked before every handoff, not only after flashing.
- A board ID in the header prevents flashing an image built for another board.
- **Boot-loop guard** (implemented, issue #1):
  1. The bootloader increments `mailbox.boot_count` before each handoff.
  2. The app clears it after it has run for ~5 s.
  3. If `boot_count` ≥ 3 at reset, the bootloader stays in update mode and sets `mailbox.reason` = 2. Staying for any reason zeroes the count, so `REBOOT(0)` gives the app 3 fresh tries.
  4. SRAM is random after power-on. The bootloader reads and clears `GCR_RSTSRC`. On POR, LVR or BOD it zeroes `boot_count` and `reason`. A random `'BOOT'` magic is still caught by the inverse-copy check.
  5. Only warm resets that reach LDROM count. `ISPCON.BS` is kept over warm resets, so today that is the app's own reboots (`sys_reset_to(1)`). With the app setting `BS=1` at startup, watchdog and `SYSRESETREQ` resets would count too; no app does that yet. A hang without a reset isn't caught.
- Flash endurance (10k cycles) is fine for development; even 50 flashes a day lasts more than six months per page. Config writes must be journaled.

### 3.7 Size budget (bare metal, arm-none-eabi-gcc -Os -flto, no newlib) — estimates

| Part | MIDI bootloader | CDC variant |
|---|---|---|
| vectors (16 core + IRQ slots used) + startup, no `.data` copy | 100–200 B | same |
| clock init (HXT, PLL 48 MHz, USB clock) | 80 B | same |
| USB device core: bus reset, EP0 state machine, GET_DESCRIPTOR (multi-packet), SET_ADDRESS (deferred), SET_CONFIGURATION, GET_STATUS, CLEAR_FEATURE, polled | 700–900 B | same |
| descriptors (device 18 + config 101 for MIDI / 67 for CDC + strings) | ~200 B | ~170 B |
| class glue: USB-MIDI event pack/unpack (CIN 4–7) / CDC line coding | ~150 B | ~100 B |
| protocol: SysEx framing, 8→7 decode, checksum, dispatch, replies | ~300 B | ~200 B (binary frames) |
| FMC: read, page erase, program, ISPFF check | ~120 B | same |
| CRC32 (nibble table) | ~100 B | same |
| entry check + mailbox + LED blink + handoff | ~150 B | same |
| literal pools, alignment, misc | ~200 B | same |
| **Total** | **≈ 2.0–2.5 KB** | **≈ 1.8–2.3 KB** |

RAM: one SysEx receive buffer (~320 B), USB state (~64 B) and a 512 B stack, under 1 KB in total.

Evidence that this is realistic:
- the stock Medeli LDROM does USB-MIDI SysEx updates in 4,080 B *with* the Keil C runtime and a 1,280 B ring buffer;
- Nuvoton's ISP_HID fits in 4 KB LDROM with the generic BSP USB driver;
- majbthrd's SAMD11/21 DFU bootloader is 1 KB.

---

## 4. Browser API landscape (Oct 2026)

| API | Chrome / Edge / Opera (desktop) | Firefox | Safari | Android Chrome | Linux extra setup | Windows extra setup | macOS |
|---|---|---|---|---|---|---|---|
| **Web MIDI** (+SysEx) | yes (permission prompt) | **yes since 108**, gated behind a generated site-permission add-on; `sysex:true` triggers the same flow | **no** (only a third-party Safari extension) | yes | none (ALSA) | none. Windows MIDI Services (GA Feb 2026, rolling out to 11 24H2/25H2) makes ports multi-client; on older stacks one app at a time | none |
| **Web Serial** | yes | **yes since 151 (May 2026)** | no | (no) | user in `dialout` | none (usbser.sys for CDC-ACM) | none |
| **WebUSB** | yes | no | no | yes | udev rule | WinUSB needed (MS OS 2.0 descriptors) | none |
| **WebHID** | yes | no | no | no | udev rule (hidraw) | none | none |

- **One page for flashing and config:** Web MIDI is the only API that covers both with a single permission, in both Chromium and Firefox, with nothing to install on any desktop OS.
- **The page:** must be served over HTTPS (GitHub Pages, as fcb1010 already does).
- **Safari users:** need Chrome/Edge/Firefox, or the Python tool.

---

## 5. App-side USB and the debug log (steps 2–4)

Budget: **4 data slots** after control.

| Option | Slots | Log viewer | Config path | Verdict |
|---|---|---|---|---|
| **MIDI only; logs as SysEx `LOG`/`EVENT` messages** | 2 | gemifi page log pane; `gemifi.py log`; any MIDI monitor | SysEx | **Recommended default.** No extra code, works in Firefox, and the same port as config and flashing |
| MIDI + CDC-ACM | 5 | — | — | **Doesn't fit** |
| MIDI + CDC without notification EP | 4 | — | — | Linux `cdc-acm` rejects it (`bNumEndpoints == 0` → `-EINVAL`). Windows/macOS behaviour **(unverified)**. Don't. |
| **Debug personality: CDC-ACM + MIDI IN-only** (compile flag) | 3 + 1 = 4 | any serial terminal / Web Serial | text commands over CDC (incl. `boot` → reboot to bootloader); notes still go out over MIDI | **Fallback if a real tty is wanted.** No SysEx config in this build |
| MIDI + vendor bulk IN (WebUSB) for logs | 3 | WebUSB page | SysEx | Chromium only, needs WinUSB descriptors and a Linux udev rule. Not worth it |
| MIDI + HID IN (WebHID) for logs | 3 | WebHID page | SysEx | Chromium only, Linux udev rule. Not worth it |
| MIDI + DFU runtime (detach) | 2 (DFU uses EP0) | — | — | Free to add later if option C is ever adopted |
| Bit-banged UART TX on PB.15 (pin 43) | 0 | FT232RL + terminal | — | Only if PB.15 reaches a pad (`hw-map.md` open question 5). Very useful for **USB bring-up**, when USB itself is what's broken |
| SWD semihosting / RTT-style RAM ring | 0 | OpenOCD | — | Works today, but slow on the Bus Pirate. Better with the Pico debugprobe |

**USB SRAM budget (512 B) for MIDI-only:**
- setup 8 B;
- EP0 IN and OUT, 64 + 64 B;
- MIDI IN and OUT, 64 + 64 B.

That's 264 B. The debug personality (EP0 128 + CDC notify 8/16 + CDC 64 + 64 + MIDI IN 64) is about 400 B, which also fits.

**App USB stack:** either reuse the bootloader's bare-metal USB core (same descriptors code, ~1 KB) or use TinyUSB (§7). Reusing ours keeps one well-understood driver and leaves the most room in 32 KB / 4 KB RAM. TinyUSB's `dcd_nuc120.c` (MIT) is the best register-level reference either way.

---

## 6. SysEx protocol (config, logs, firmware)

**ID:**
- **0x7D** (the MIDI "non-commercial" special ID) while gemifi is a hobby/dev project, followed by a 2-byte project signature to avoid collisions with other 0x7D users.
- MIDI Association rules: **0x7D must not be used in products released to the public**.
- The new **0x7C** "limited non-commercial" ID (Oct 2025) *does* allow public release, but explicitly **not** for SysEx with custom or manufacturer-defined data formats. So it doesn't fit.
- If gemifi images are ever distributed widely, buy a 3-byte manufacturer ID. Keep the ID a build constant so it can be swapped.

**Frame** (all bytes 7-bit):
```
F0 7D 67 6D  ver  cmd  seq  len_lo len_hi  payload…  chk  F7
             │    │    │    └ payload length in SysEx bytes (14-bit)
             │    │    └ 0..127, echoed in the reply
             │    └ request 0x00–0x3F; reply = cmd | 0x40; unsolicited device events 0x60–0x7F
             └ protocol version (1)
chk = (sum of bytes from ver through last payload byte) & 0x7F
```
- **Binary data:** packed **8→7, 7 bytes into 8**: a leading byte with the seven MSBs, then the seven low-7-bit bytes. This is the common Korg/Elektron scheme, 14% overhead.
- **Integers:** 7-bit groups, least significant first. 32-bit = 5 bytes; 28-bit offsets = 4 bytes, the same as the stock Medeli updater.
- **Replies:** `cmd|0x40` with a status byte first: 0 OK, 1 BAD_CHECKSUM, 2 BAD_CMD, 3 BAD_LEN, 4 BAD_STATE, 5 BAD_ADDR, 6 FLASH_ERR, 7 BAD_IMAGE, 8 BAD_BOARD, 9 BUSY.
- **Flow:** strictly stop-and-wait. One outstanding request per `seq`, retry on a ~200 ms timeout.

| cmd | Name | In app | In bootloader | Payload → reply |
|---|---|---|---|---|
| 0x01 | HELLO | ✓ | ✓ | → role (0 = app, 1 = boot), board_id, fw_version, proto_version, max_chunk, capability flags, chip UID |
| 0x02 | GET_SCHEMA | ✓ | — | offset, len → schema bytes (packed). Schema is a compact binary blob in APROM describing params (id, type, range, label, group); the editor renders forms from it (`framework-research.md` §3) |
| 0x03 | GET_PARAM / 0x04 SET_PARAM | ✓ | — | param id (14-bit), index, value (up to 32-bit) |
| 0x05 | GET_BLOB / 0x06 SET_BLOB | ✓ | — | offset, packed bytes. Raw profile read/write for backup/restore |
| 0x07 | COMMIT | ✓ | — | write the RAM profile to data flash (journal) |
| 0x08 | FACTORY_RESET | ✓ | (opt.) | — |
| 0x0F | LOG_LEVEL | ✓ | — | min level, event mask |
| 0x10 | REBOOT | ✓ | ✓ | mode: 0 app, 1 bootloader |
| 0x20 | FW_BEGIN | — (app replies BAD_STATE + "reboot first") | ✓ | size, crc32, board_id, fw_version → max_chunk |
| 0x21 | FW_DATA | — | ✓ | offset (28-bit), packed ≤ 256 B |
| 0x22 | FW_END | — | ✓ | → OK / BAD_IMAGE (CRC) |
| 0x23 | FW_ABORT | — | ✓ | — |
| 0x60 | **LOG** (device → host, unsolicited) | ✓ | (opt.) | level, ms timestamp (28-bit), ASCII text (7-bit safe, ≤ 64 chars) |
| 0x61 | **EVENT** (device → host) | ✓ | — | type (1 = button, 2 = key, 3 = pot…), index, value, timestamp. Step 2's "button pressed" is `EVENT button idx=2 val=1` plus an optional `LOG "oct- down"` |

The app also answers the **Universal Identity Request** (`F0 7E 7F 06 01 F7`) with family/model/version, so generic tools and the page can find it before speaking gemifi SysEx.

**Comparison with OpenDeck** (shanteacontrols/OpenDeck, Apache-2.0):

**Config protocol.** Checked against its wiki:
- ID `00 53 43`. Fixed layout: `status, part, wish(GET/SET/BACKUP), amount(single/all), block, section, index(2 B), value(2 B)`.
- Values are 14-bit in 2 bytes. Up to 32 values per message.
- A handshake (`F0 00 53 43 00 00 01 F7`) is required before other requests.
- Special requests: firmware version 0x56, reboot to bootloader 0x55, bootloader support 0x51, backup 0x1B. Error codes 0x02–0x0E.
- What we take: the handshake-free but versioned HELLO, numeric status codes, and the backup/restore idea.
- What we don't take: the block/section addressing. A self-describing schema with flat param IDs is more general.

**Firmware updates.** Two generations:
- **Legacy v7:** a SysEx bootloader (`src/firmware/bootloader/sysex_parser`). Each firmware byte is split into two 7-bit bytes (50% efficiency), 32 bytes per message, streamed with no per-message ACK.
- **Current v8 (2025–26):** moved to **MCUboot + a recovery image over WebUSB/WebSockets**, with Begin/Chunk/Finish/Abort frames (61-byte WebUSB chunks) and an ACK `0x81 cmd status bytes_written` after each command.
- Lessons: per-chunk ACKs and an explicit Begin/Finish are what they settled on. We keep both, inside SysEx.
- Their move away from SysEx was driven by larger images and network targets, which don't apply to a 32 KB part.

**Stock Medeli updater** (`ldrom.md` §3), for comparison:
- 9-byte header `00 20 2B 'MIDI' 7F`;
- byte-sum checksums;
- 512 B pages as (lo7, hi1) pairs;
- reply codes 04–07.

It confirms that 512 B-page SysEx writes work on this exact USB/flash setup.

---

## 7. TinyUSB on NUC120 (for the app, optional)

- **Port:**
  - `src/portable/nuvoton/nuc120/dcd_nuc120.c` (MIT, Peter Lawrence 2019), maintained in tree. Last functional changes: 2025-10 (`is_isr` API) and 2026-04/07 (warnings, SPDX).
  - TinyUSB release **0.21.0 (2026-06-30)**. Board: `hw/bsp/nuc100_120/boards/nutiny_sdk_nuc120`.
- **Known gaps:** `dcd_edpt_close`/`close_all` and the FIFO transfer API are not implemented; issue #1029, "merge nuc120/nuc121 dcd", is open. There are no open nuc120 bug reports. It was presumably tested on the NuTiny board only.
- **BSP dependency:** **yes**. The family build pulls Nuvoton's `NUC100Series.h`, `system_NUC100Series.c`, the GCC startup and StdDriver `clk/gpio/sys/timer/uart` from `majbthrd/nuc_driver` (a 2020 copy of Nuvoton driver files; the repo has no licence file, but the files carry Nuvoton headers). The driver itself only needs the device header.
- **Board mismatch:** the board files target **NUC120LE3** (128 KB flash / 16 KB RAM, linker `nuc120_flash.ld`). We need our own 32 KB / 4 KB linker script. TinyUSB's makefile flashes through **OpenNuvoton's OpenOCD fork + Nu-Link**; we'd use upstream OpenOCD instead.
- **Footprint estimates (Cortex-M0, -Os, LTO, `CFG_TUSB_DEBUG=0`), not measured:**

  | Configuration | Flash | RAM |
  |---|---|---|
  | MIDI | ~6–8 KB | ~0.6–0.8 KB (EP0 buffer, MIDI FIFOs 64+64, endpoint buffers) |
  | MIDI + CDC | ~8–10 KB | — (doesn't fit the endpoint budget) |
  | MIDI + vendor | ~7–9 KB | — |

  TI's TinyUSB guide reports 10–15 KB for CDC/HID examples at -O2 *with* their driverlib and board code, which brackets these numbers.
- **SRAM:** 4 KB is enough for TinyUSB MIDI, a 1 KB stack, a ~300 B key-matrix state, the SysEx receive buffer and a log ring.

---

## 8. Toolchain and SWD commands

- **Compiler:** `gcc-arm-none-eabi` + `libnewlib-arm-none-eabi` (Ubuntu packages, not installed yet). Flags: `-mcpu=cortex-m0 -mthumb -Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections`, plus `-nostartfiles -nostdlib` for the bootloader.
- **Headers:** **OpenNuvoton/NUC100BSP** on GitHub (Apache-2.0; `NUC100Series.h` V3.0; last commit 2024-08). It has GCC startup and a linker script under `Library/Device/Nuvoton/NUC100Series/Source/GCC/`, plus the ISP samples. The ISP samples are Keil-only. Use the device header; write the bootloader against registers, not StdDriver.
- **Linker scripts:**
  - **bootloader:** `FLASH ORIGIN=0x00000000 LENGTH=4K`, `RAM ORIGIN=0x20000000 LENGTH=4K-16`; programmed at 0x00100000.
  - **app:** `FLASH ORIGIN=0 LENGTH=32K`, same RAM; `.gemifi_hdr` section placed at 0xC0 (`KEEP`).
- **OpenOCD 0.12 `numicro`:**
  - **Banks:** APROM 0x0, data 0x1F000, LDROM 0x100000, CONFIG 0x300000 (the existing `openocd-buspirate.cfg` declares all four).
  - **Commands:** `numicro read_isp <addr>`, `numicro write_isp <addr> <value>`, `numicro chip_erase`.
  - **ISP setup:** the driver sets ISPEN, LDUEN, APUEN and CFGUEN itself while halted.

```sh
# app → APROM (keeps stock LDROM + CONFIG: milestone 1)
openocd -f openocd-buspirate.cfg -c init -c halt \
  -c "flash write_image erase build/app.bin 0x00000000" \
  -c "verify_image build/app.bin 0x00000000" -c reset -c shutdown

# gemifi bootloader → LDROM (milestone 3; stock copy is backup/ldrom.bin)
openocd -f openocd-buspirate.cfg -c init -c halt \
  -c "flash write_image erase build/boot.bin 0x00100000" \
  -c "verify_image build/boot.bin 0x00100000" -c reset -c shutdown

# Restore stock
#   ... "flash write_image erase backup/aprom-read1.bin 0x0" and "... backup/ldrom.bin 0x00100000"

# CONFIG0 (NOT needed for this design). NOTE (2026-10-30): in OpenOCD 0.12 the CONFIG bank has zero
# 512-byte sectors, so `flash erase_address 0x00300000 0x200` FAILS. Use tools/swd/nuvoton-nuc1xx/restore-stock.sh --write-config
# (a raw ISP page erase + program with read-back) instead. Original sketch, kept for reference:
#   -c "numicro read_isp 0x00300000" -c "flash erase_address 0x00300000 0x200" \
#   -c "numicro write_isp 0x00300000 0xFF4FFF7F" -c "numicro write_isp 0x00300004 0xFFFFFFFF"
```

CONFIG notes:
- Setting CBS=1 (APROM boot) would be `0xFF4FFFFF`.
- An interrupted CONFIG erase leaves all-ones, which means: APROM boot, internal RC, unlocked, no BOD. That state is benign.
- `verify_image` only works on memory the core can read, which excludes CONFIG. Check CONFIG with `read_isp`.

**Pico debugprobe:** `-f interface/cmsis-dap.cfg -c "transport select swd" -c "adapter speed 1000"` plus the same target and flash bank lines. It should be orders of magnitude faster than the Bus Pirate's ~80 B/s.

---

## 9. Development loop

- **Build:** `make` produces `build/app.elf` → `app.bin` → `tools/mkimage.py` fills the header (size, CRC, version) → `build/app.gfw`.
- **`tools/gemifi.py`** (Python 3, `python-rtmidi`; on Linux it can also open `/dev/snd/midiC*D0` directly with no dependencies):
  - `gemifi.py flash build/app.gfw`:
    1. Find the "gemifi" port.
    2. HELLO.
    3. REBOOT(bootloader).
    4. Wait for the "gemifi boot" port (≤ 3 s).
    5. FW_BEGIN / DATA… / END.
    6. REBOOT(app).
    7. Wait for "gemifi" and print HELLO.

    About 4–6 s end to end. If no app port is present, it goes straight to the boot port (fresh board, or a combo entry).
  - `gemifi.py log`: prints LOG/EVENT messages (`[  1234 ms] INFO oct- down`).
  - `make run` = build + `flash` + `log`.
- **Web page** (fcb1010 fork, TypeScript): the same protocol module. A **Device** tab (HELLO info, Flash firmware from a `.gfw` URL or file, with a progress bar and CRC check before sending), a **Log** pane, and later the schema-driven **Config** tabs.
- **SWD** is only for the bootloader itself and for recovery.

---

## 10. First-milestone plan

Smallest path that proves "USB in, log out, reboot-to-bootloader", without touching LDROM or CONFIG:

1. **Prep:**
   - install `gcc-arm-none-eabi`;
   - vendor `NUC100Series.h` from NUC100BSP;
   - solder or prepare the /RESET (pin 46) wire;
   - ideally have the Pico debugprobe ready.
2. **M1, "hello LED" app in APROM over SWD.**
   - Code: 48 MHz clocks, Oct− LED (PA14 / PA10, active-low) blinking, Oct+ LED lit while Oct+ (PA4) is held.
   - Flash only APROM. Stock LDROM (CBS=0) hands off unconditionally, so this also proves the LDROM → APROM handoff with our startup code.
   - Restore = `backup/aprom-read1.bin`.
3. **M2, bare-metal USB-MIDI app:**
   - The USB core shared with the future bootloader, with MIDI descriptors (1209:test PID).
   - Each button press sends a note plus a `LOG`/`EVENT` SysEx.
   - It answers HELLO and the Identity Request.
   - It implements `REBOOT(bootloader)`. With the stock LDROM still installed, the reboot lands in stock LDROM, which immediately hands back to the app. That exercises the mailbox, disconnect and re-enumeration path harmlessly.
   - Write `gemifi.py log`. **This completes roadmap step 2.**
4. **M3, bootloader as an APROM test build:**
   - The same bootloader source linked at 0x0 and flashed into APROM.
   - It enumerates as "gemifi boot" and runs the full FW_* protocol, but writes into **data flash** as a stand-in target, because APROM can't write itself.
   - This validates the USB, SysEx and CRC code and the host tool without any LDROM risk.
5. **M4, bootloader into LDROM over SWD** (CONFIG unchanged), then the app over USB with `gemifi.py flash`. Test:
   - (a) the combo at plug-in;
   - (b) REBOOT(bootloader) from the app;
   - (c) a corrupted image (flip a byte) staying in the bootloader;
   - (d) pulling USB mid-flash, then recovery;
   - (e) writes to LDROM, CONFIG or data-flash addresses being rejected.
6. **M5, web page flasher** (Web MIDI) in the fcb1010 fork: Device and Log tabs. **This completes roadmap steps 1 and 3.** Step 4 (schema-driven config) builds on the same protocol module.

---

## 11. Open questions and risks

- **APUEN on this die.** Is ISPCON bit 3 implemented on NUC120LC1BN? This can be read and written safely in RAM-only tests. It doesn't change the recommendation, but it would allow in-app config of the APROM.
- **Combo choice.** Pitch▼ + Pitch▲ (PA5, PA6) assumes the pin order in `hw-map.md`; confirm on hardware.
  - It doesn't clash with stock combos (Oct− + Oct+ = factory reset; Sustain + Edit + Oct− = exit main).
  - With our app the stock combos no longer matter, but avoiding them keeps behaviour unsurprising if stock firmware is ever restored.
- **SRAM across CPU_RST.** SRAM should survive CPU_RST (expected, not verified) and REGWRPROT may stay unlocked (`ldrom.md`). Verify in M2.
- **USB re-enumeration after CPU_RST.** The USB block isn't reset by CPU_RST, so the disconnect (DRVSE0) must be long enough for every host. Test Windows, macOS and Linux.
- **Windows MIDI.** Port-name caching and multi-client behaviour on pre-Windows-MIDI-Services systems: the DAW must release the port during a flash. Separate PIDs help.
- **Firefox Web MIDI add-on flow** with `sysex:true`: check it is acceptable UX. Chrome also shows its own MIDI permission prompt.
- **Max SysEx size through each OS stack.** We use ≤ ~330-byte messages, which is conservative; verify on Windows MIDI Services and macOS CoreMIDI.
- **Bootloader bugs need SWD.** Keep the LDROM code tiny and test it in APROM first (M3). Keep `backup/ldrom.bin` for restore.
- **IDs.** A pid.codes PID and the 0x7D ID are fine for development. Public distribution needs a real SysEx manufacturer ID (0x7C doesn't allow custom data formats).
- **Stock updater.** The stock updater's command 3 decode has an unresolved caveat (`ldrom.md` §3). Don't rely on it as a recovery path; we replace it anyway.
- **PB.15 for a debug TX pad** (`hw-map.md` Q5): it would give a USB-independent log during M2 bring-up.

---

## Sources

- NuMicro NUC100/NUC120 Technical Reference Manual V2.02: USB device controller (§5.4, USB_CFGx, 512 B buffer, 6 endpoints) and FMC (§6: ISPCON, ISPFF conditions, LDUEN, CBS/BS, ISP procedure). Local copy: `scratchpad/ds/trm.txt`. Datasheet flash specs (page erase 20–40 ms, endurance 10k): `scratchpad/ds/nuc120.txt`.
- Nuvoton NUC100 BSP (Apache-2.0): https://github.com/OpenNuvoton/NUC100BSP. Used: `SampleCode/ISP/ISP_HID` (`isp_user.[ch]`, `main.c`, `hid_transfer.h`, `KEIL/Nu_Link_Driver.ini` FlashSelect=LDROM), `SampleCode/ISP/ISP_DFU`, `SampleCode/StdDriver/FMC_IAP` (0x20000FFC signature), `Library/StdDriver/inc/fmc.h` (APUEN macro).
- NuMicro ISP Programming Tool (Apache-2.0): https://github.com/OpenNuvoton/ISPTool
- TinyUSB: https://github.com/hathach/tinyusb. Used: `src/portable/nuvoton/nuc120/dcd_nuc120.c`, `hw/bsp/nuc100_120/`, `tools/get_deps.py` → https://github.com/majbthrd/nuc_driver; release 0.21.0; issue https://github.com/hathach/tinyusb/issues/1029
- TI MSPM0 TinyUSB User Guide (code-size table): https://software-dl.ti.com/msp430/esd/MSPM0-SDK/latest/docs/english/middleware/tinyusb/TinyUSB_User_Guide.html
- SAMDx1 1 KB USB DFU bootloader: https://github.com/majbthrd/SAMDx1-USB-DFU-Bootloader
- UF2 SAMD bootloader (8 KB region): https://github.com/microsoft/uf2-samdx1
- webdfu: https://github.com/devanlai/webdfu
- OpenDeck: SysEx config protocol https://github.com/shanteacontrols/OpenDeck/wiki/Sysex-Configuration; v7 SysEx bootloader `src/firmware/bootloader/sysex_parser/*` and `src/tools/sysexgen/main.cpp` at tag v7.12.2; v8 bootloader README `app/bootloader/README.md` (MCUboot + WebUSB/WebSockets framing). Repo: https://github.com/shanteacontrols/OpenDeck
- OpenOCD 0.12 numicro driver: https://github.com/openocd-org/openocd/blob/v0.12.0/src/flash/nor/numicro.c
- Linux `cdc-acm` probe (rejects a control interface with no endpoints): https://github.com/torvalds/linux/blob/master/drivers/usb/class/cdc-acm.c
- Browser support: https://caniuse.com/midi, https://caniuse.com/webusb, https://caniuse.com/webhid; MDN Web MIDI https://developer.mozilla.org/en-US/docs/Web/API/Web_MIDI_API; Firefox Web MIDI site-permission add-on: https://bugzilla.mozilla.org/show_bug.cgi?id=1752906, https://bugzilla.mozilla.org/show_bug.cgi?id=1806056, https://searchfox.org/firefox-main/source/dom/midi/tests/test_midi_permission_gated.html; Firefox Web Serial (151, May 2026): https://theregister.com/2026/04/14/firefox_nightly_web_serial, https://korben.info/en/firefox-nightly-web-serial-api.html
- Windows MIDI Services GA (Feb 2026, multi-client): https://blogs.windows.com/windowsexperience/2026/02/17/making-music-with-midi-just-got-a-real-boost-in-windows-11/
- MIDI Association SysEx ID policy (0x7D, 0x7C): https://midi.org/new-midi-association-sysex-id-policies-as-of-oct-15-2025
- Project analyses: `ldrom.md` (stock LDROM: USB-MIDI SysEx updater, CPU_RST handoff), `hw-map.md` (buttons PA1–PA7, LEDs PA10–15, no UART, PB.15 free, stock power-up combos).
