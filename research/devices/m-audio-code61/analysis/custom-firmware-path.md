# Code 49/61: how to get gemifi onto the keyboard

Written 2026-11-01. Static analysis only, of the Code 49 v1.2.1 update file and app image (see [update-protocol.md](update-protocol.md)). No hardware was touched. Notes that quote vendor bytes or disassembly are kept locally in `private/` (gitignored).

**Question.** Can gemifi replace only the app area (0x08002400 and up) through the stock bootloader's update protocol? Or do we need our own bootloader, installed over SWD as on the Keystation Mini 32?

**Short answer.** The app-only route **looks feasible**. Nothing in the update path is encrypted or signed. We now know every check value the update stream carries, including a whole-image checksum that was missed before. We also know how the stock app asks the bootloader for update mode. What's still unknown is the bootloader itself (we have no dump), so three things need a hardware test: whether it accepts a stream we build, whether it really checks the image checksum at boot, and whether it has a button combo for recovery. Recommended path: install via the vendor protocol, and keep SWD/BOOT0 for development units and as the last-resort unbrick.

## 1. Update format and transport

Already covered in [update-protocol.md](update-protocol.md): plain SysEx carrying uppercase ASCII hex, an 11-byte record chunk per message, and a per-record `F0 cc F7` acknowledgement. **There is no encryption or scrambling.** The data records carry the app bytes verbatim; `scripts/update_protocol.py rebuild` turns the plain app back into a byte-identical stream.

New in this pass:

**Records are lists of 6-byte commands.** A record has a 2-byte length, then a 4-byte constant (`6A 04 DA 3B`), then commands of the form `op, page (u16 BE, ×0x100 + 0x08000000), count (u16 BE, ×256 bytes)`. After the commands come four zero bytes and the 16-bit checksum.

| op | Inferred meaning | Used in |
|---|---|---|
| `09` | erase the 1 KB page at `page` | every record (first command) |
| `0A` | load `count`×256 bytes (they follow the command) into the buffer | data records |
| `0B` | program the buffer at `page` | data records |
| `0D` | probably verify (it targets the checksum page) | final record only |
| `0E` | probably finish: clear the update request, then reset or jump | final record only |

So the "erase records" from the earlier notes are just records holding a single `09` command.

**There is a whole-image checksum** (new). The final record writes the page at 0x0801B000. That page is all `FF` except its last halfword, at **0x0801B3FE**. That halfword is the negated 16-bit sum of every byte from **0x08002400 to 0x0801B3FD**, with unwritten flash counted as `FF`, stored little-endian. It's the same arithmetic as the per-record checksum. It matches the shipped v1.2.1 value exactly, and it is stored at the very last address the stream writes. That makes it almost certainly the bootloader's "app valid" marker, written last so that a power cut leaves the image invalid. The earlier README's "small table at the end" of that page was a misreading; there is no table.

**Signatures: none.** The only unexplained constant is the 4-byte record signature `6A 04 DA 3B`. It isn't in the updater exe, only in the firmware file. It may be a product or model ID that the bootloader checks. If so, the Code 25/49/61 values may differ (see §7).

**Can we build a valid stream for any image?** Yes, as far as the file format goes. Erase and write every page from 0x08002400 to 0x0801AFFF, then write the final page with the recomputed checksum and the `0B/0D/0E` tail. That is what the stock file does. `update_protocol.py rebuild` doesn't yet recompute the image checksum, which it must do before any non-stock image is sent (§8).

## 2. What the bootloader probably checks

From the stream (inferred, not seen):

- **The per-record checksum** and probably the record signature: one 16-bit sum per record, with an ack or NAK per record.
- **An image checksum over 0x08002400–0x0801B3FD**, compared against 0x0801B3FE, at least when it receives `0D`. Possibly at every boot, too: that's the usual reason to store it in flash.
- **No signature, CRC-32 or version field** that we can see in the app image. The app has no header at all: its vector table starts at 0x08002400.
- **Board/model.** Nothing in the app area identifies the model. If the bootloader tells 49 and 61 apart, it does so through the record signature or its own constants.

## 3. How the bootloader is entered

**From the app (found this time; the earlier notes said no handler existed).**

1. The stock app watches its USB-MIDI OUT endpoint for exactly the start message `F0 5A 57 6E 28 3C 4E 45 F7`, as three raw USB-MIDI packets. The handler is at 0x0800748C, called from the endpoint code at 0x08010F5E.
2. On a match, it writes an **8-byte magic word to the first bytes of the last 1 KB flash page, 0x0801FC00**, and fills the rest of that page with `FF`.
3. It answers `F0 15 F7`.
4. It does **not** reset. There's no SYSRESETREQ anywhere in the app.

This explains the updater's "Turn the device off and on, then press NEXT" step. At power-up, the bootloader sees the magic in 0x0801FC00 and stays in update mode. The updater's 2 s probe (the same start message) is then answered by the bootloader with `F0 45 F7`. The update stream never writes 0x0801FC00, so the bootloader must clear the request itself, most likely on `0E`.

**Button combo at power-up:** unknown. The ReadMe and updater strings mention none.

**If the app is invalid:** unknown, but likely. The image checksum is there for this reason.

**What that means for recovery** if gemifi replaces the app:

- **Interrupted or corrupt upload:** probably safe. The request flag at 0x0801FC00 is still set, and the image checksum is missing or wrong, so the next power-up should land in the vendor bootloader. Then re-send gemifi or the stock stream.
- **A gemifi build that boots but hangs or crashes before it can handle SysEx:** the checksum is valid and no flag is set, so the bootloader jumps into the broken app every time. Unless the vendor bootloader has a button combo, the only ways out are SWD or the ST ROM bootloader (BOOT0). **This is the real bricking risk of the app-only route**, and gemifi has to defend against it (§5).
- **gemifi requesting an update itself:** gemifi can do what the stock app does: write the same magic word to 0x0801FC00, then reset. Unlike the stock app, gemifi *can* reset itself (SYSRESETREQ). Whether the bootloader checks the flag after a soft reset, or only after power-on, needs a test; the fallback is "power-cycle now".

## 4. App contract (what the stock app shows us)

- **Vector table** at 0x08002400. Initial SP 0x200040B0, reset handler 0x08002545, 60 external IRQ slots.
- **VTOR:** the app sets it itself. Early in init it calls the standard NVIC "set vector table" helper with 0x08000000 + 0x2400 (0x0801199C, called from 0x08012A1C). So the bootloader doesn't have to set VTOR, and gemifi should set it too.
- **Clocks:** the app does a full RCC de-init, then HSE ×9 = 72 MHz with 2 wait states (0x08011C14). It assumes nothing about the bootloader's clock state. gemifi should do the same, and also reset USB and pulse the D+ pull-up before enumerating, since the bootloader may leave USB running.
- **Watchdog:** the app starts the IWDG with a timeout of about 1 s (0x08011458, from main). The bootloader may or may not start it; gemifi should start it as well.
- **Flash page size is 1 KB.** The update erases page by page in 0x400 steps, and the app's settings writer refuses offsets that aren't 1 KB aligned. If the pages were 2 KB, erasing 0x0801B000 would wipe the calibration at 0x0801B400. ST's high-density F103 parts have 2 KB pages, but the app touches high-density-only peripherals (UART5, DMA2). So **the MCU may not be a genuine ST part** (a GD32F103 or another clone would fit), or the earlier "high-density" call is wrong. This matters for OpenOCD config and for flash code. Settle it with a board photo.
- **Flash map:**

| Range | Owner | gemifi app-only rule |
|---|---|---|
| 0x08000000–0x080023FF | vendor bootloader (9 KB) | never write |
| 0x08002400–0x0801AFFF | app | gemifi code; ~99 KB max |
| 0x0801B000–0x0801B3FF | app-valid page: `FF`s + image checksum at 0x0801B3FE | written last by every update |
| 0x0801B400–0x0801FBFF | vendor settings "EEPROM" (18 × 1 KB), includes per-unit calibration | read calibration; don't erase the vendor's pages |
| 0x0801FC00–0x0801FFFF | update-request flag page | write only to request update mode |

- **Max app size:** 0x08002400–0x0801B3FD, i.e. 102,398 bytes.
- **Settings must live outside the checksummed range.** If gemifi kept its settings inside 0x08002400–0x0801B3FD, every save would invalidate the image checksum. So they belong in the vendor EEPROM area, in pages the stock firmware doesn't use. Which pages those are hasn't been mapped yet, and a return to stock firmware must still find its own pages intact.
- **RAM:** the stock app uses up to about 0x20004900. Nothing suggests the bootloader keeps RAM alive across the jump, because the update request lives in flash rather than RAM.

## 5. The app-only route for gemifi

**What we'd build:**

- An STM32F1 platform for gemifi, with the app linked at 0x08002400.
- The vendor upload as a second transport in `tools/host` and `web/updater`: Web MIDI with SysEx, start message, records as ASCII-hex chunks, a wait for each ack.
- A packer that turns a gemifi image into a record stream and writes the image checksum.

None of it ships vendor code. The stream format, the 4-byte signature and the magic word are protocol constants. We'd ship only our own image, and the user's keyboard keeps its own bootloader.

**gemifi into vendor bootloader.** Yes. gemifi's `REBOOT(1)` on this board means: write the magic to 0x0801FC00, then reset or ask for a power cycle. The web updater then speaks the vendor protocol to the bootloader. `HELLO` from the gemifi app would report role 0 with no upload cap, plus a board-specific hint that uploads go through the vendor protocol. That hint would be a small extension to sysex.md.

**Recovery hardening.** This replaces the button check the gemifi bootloader does on the Keystation.

1. The very first thing in the reset handler, before clocks, USB or settings: sample a recovery button combo, e.g. both octave buttons. If it's held, write the 0x0801FC00 magic and reset. This uses only GPIO and the flash controller, so a bug elsewhere can't break it. It's kept as a separate, never-changing startup file.
2. IWDG on from early in boot, so a hang becomes a reset rather than a frozen keyboard.
3. Optional, for development builds: set the flag at boot and clear it once USB has run cleanly for a few seconds. A crash loop then drops into the vendor bootloader on the next power cycle. It costs one page erase per boot, so keep it out of release builds.

**Risks:**

| Risk | Assessment |
|---|---|
| Bootloader rejects our stream (signature, model, unknown check) | Unknown until tested. Low cost: test it first with the stock stream sent by our own tool |
| Brick through a bad gemifi build | Real. Mitigated by the recovery combo plus IWDG; SWD/BOOT0 as the backstop |
| Power loss mid-update | Vendor design: flag still set, checksum written last, so it probably stays recoverable |
| Readout protection (RDP) | Unknown. It doesn't matter for this route: RDP doesn't stop the vendor bootloader programming the app. It does matter for SWD (below) |
| Clone MCU quirks | 1 KB pages hint at a non-ST part. Board photo needed |
| SWD exposed on the PCB? | Unknown. No board photo yet (README "Needs hardware") |
| Legal | Best of the three routes: we ship and flash only our own code |

## 6. Recommendation

| Option | Install | Recovery | Vendor code | Risk | Verdict |
|---|---|---|---|---|---|
| **(a) App-only via the vendor bootloader** | Web MIDI, no tools, case stays closed | Vendor bootloader (flag page), gemifi recovery combo, SWD/BOOT0 last | none shipped | Low if the stream is accepted; a bad build can need SWD | **Do this first** |
| (a′) gemifi bootloader *in the app area* (0x08002400), gemifi app after it | Same as (a), once | Vendor bootloader below it stays untouched; gemifi bootloader gives HELLO/FW_* parity with other boards | none | Like (a), but the gemifi bootloader must rewrite the vendor image checksum after each app upload, and ~8 KB less app space | **Later, if we want the gemifi protocol everywhere** |
| (b) Stage-1 app rewrites 0x08000000 with a gemifi bootloader | Vendor protocol, then self-rewrite | None during the rewrite; afterwards ours only | none | A power cut or bug during the rewrite bricks it; option-byte write protection unknown; vendor recovery gone for good | **Avoid** |
| (c) gemifi bootloader via SWD or ST ROM bootloader (BOOT0 + USART1) | Open the case, probe or USB-UART | SWD/BOOT0 always work | none | If RDP is set, unlocking mass-erases the vendor bootloader, permanently | **Development units and unbricking only** |

Points in favour of (a), and later (a′):

- The vendor bootloader is a recovery path we get for free, and the case never has to be opened.
- The stock app stays restorable from the user's own vendor update file, through our tool or the vendor updater.
- Even after (c), the stock app could run under a gemifi bootloader, because it sets its own VTOR and clocks.

## 7. Still unknown

1. Does the bootloader accept a stream sent by our tool? Does it accept a non-stock image whose checksum is correct?
2. Does it check the image checksum at every boot, or only on `0D`? What do `0D` and `0E` do exactly, and what's the difference between the `45` and `1A` acks?
3. Is there a button combo at power-up, and does a soft reset with the flag set enter update mode?
4. ~~Is the record signature `6A 04 DA 3B` model-specific?~~ **Answered 2026-11-02:** no. The Code 61 v1.2.2 file uses the same signature, layout and checksum rule, and our packer rebuilds it byte for byte.
5. Chip marking (ST or clone), RDP and write-protect option bytes, and whether SWD pads and BOOT0 are reachable.
6. Which vendor EEPROM pages are in use, so gemifi can pick free ones and leave the calibration alone.
7. The bootloader's USB identity in update mode (VID/PID, port name).

## 8. Hardware tests, safest first

1. **Look only.** Board photo: chip marking, SWD pads, the BOOT0 pin and its strap.
2. **Enumerate.** Send an Identity Request to the stock app and log the reply.
3. **Buttons.** Power up while holding likely combos (octave ±, transport) and watch `lsusb`/`dmesg` for a different device.
4. **Enter update mode the stock way, from our own tool.** Send the start message and expect `F0 15 F7`. Power-cycle and log the new USB identity. Watch for an unsolicited `F0 20 F7`, then send the probe and expect `F0 45 F7`.
5. **Prove the transport.** Send the stock v1.2.1 stream (`update_protocol.py syx`) from our tool and confirm the keyboard comes back on stock firmware with its settings and calibration intact. Up to here, nothing non-stock is ever written.
6. **Prove arbitrary images.** First extend `update_protocol.py` to recompute the image checksum at 0x0801B3FE. Then flash a minimal gemifi probe app. It must:
   - set VTOR, run its clocks and IWDG, enumerate USB-MIDI;
   - report the flash size register, the chip ID, the RDP/WRP option bytes and FLASH_OBR over SysEx;
   - optionally read out the vendor bootloader, privately and never committed;
   - have the recovery combo from §5 built in.
   Then return to stock via the flag page and step 5.
7. Only after that, a real gemifi app.
