# Stock LDROM bootloader (Keystation Mini 32 MK3, NUC120LC1BN)

Source: `backup/ldrom.bin` (4096 B). Because CONFIG0.CBS=0, LDROM is mapped at 0x0 on reset, so all addresses below are 0-based.
Disassembly: `"<venv>/python" -I analysis/scripts/ldrom_dis.py backup/ldrom.bin c0 ff0 0` (a copy of fwdis.py with the base address as the 4th argument).

**Short version:** the stock LDROM is a Medeli-built **USB-MIDI SysEx updater**. At reset it samples part of the key matrix. If the expected keys are **not** held, it switches to APROM straight away with `ISPCON.BS=0` and a CPU reset. It doesn't check that APROM holds a valid app. If the keys **are** held, it enumerates as USB **0A67:138F "MEDELI" / "Updater"**, a class-compliant USB-MIDI device, and accepts flash erase, program and checksum commands as SysEx with the header `F0 00 20 2B 'M' 'I' 'D' 'I' 7F`. The APROM application has no code that requests the bootloader.

## 1. Boot flow

Runtime: Keil/ARMCC (`__main` scatter-load at 0xE68/0xEF0, semihosting stubs, `BKPT 0xAB` HardFault check at 0xE98). Reset 0xF64 → 0xFA0 (SystemInit, empty) → 0xF80 `__main` → scatter-load → `main` at **0x0C0**.

`main` (0x0C0):
1. **0xDF0 clock init.** Unlocks REGWRPROT (0x50000100 ← 0x59, 0x16, 0x88) and **leaves it unlocked**. PWRCON (0x50000200) = 0x05 (12 MHz XTL + 22 MHz RC on). PLLCON (0x50000220) = **0xC22E**: 12 MHz crystal, NF=48, NR=3, NO=4 → **48 MHz**. CLKSEL0 = 0x02 (HCLK = PLL), then after a delay CLKSEL0 = 0x12 (SysTick ext src = 12 MHz/2, unused because SysTick runs from the core clock).
   Delay helper 0xDD8: SysTick LOAD = n·12, CTRL=5, busy-wait. At 48 MHz one unit is 0.25 µs.
2. **GPIO setup (0x0C6–0x160):** PB0 and PB1 are inputs (PMD=00). PA10–PA15, PB8, PB9, PC0 and PC1 are push-pull outputs. PA8 and PA9 stay in their reset mode, quasi-bidirectional. PB_PMD.PB8/PB9 = 01.
3. **Key-matrix entry check (0x162–0x1D4).** It sets four output patterns. After each it waits delay(10) (≈2.5 µs) and reads `PB_PIN & 3` (0x50004050). **If PB0 and PB1 both read 0 after any of the four samples, it calls 0x64E, which boots APROM.**

   | sample | write done before sampling | lines high at sample time (DOUT reset value is 0xFFFF) |
   |---|---|---|
   | 0x16A | PA_DOUT[15:10] = 001100b | PA12, PA13 (PA9 quasi-high, PB8/9, PC0/1) |
   | 0x186 | PA_DOUT[9:8] = 10b | PA12, PA13, PA9 (PA8 now low), PB8/9, PC0/1 |
   | 0x1A4 | PC_DOUT[1:0] = 01b | as above, with PC0 high and PC1 low |
   | 0x1BE | PC_DOUT[1:0] = 10b | as above, with PC0 low and PC1 high |

   So **update mode needs PB0 or PB1 to read high in all four samples**. This implies that pressed contacts pull a row high when their column is driven high. Idle rows read 0, otherwise the normal boot couldn't work. The APROM scan code at 0x2E4A/0x2E6C uses PA10+n as matrix columns and PB0–5/8/12 as rows, so PB0/PB1 are matrix rows. *Inferred:* the condition is "a specific key or button held on row PB0/PB1". The PA12/PA13 columns stay high in all four samples, while PC0 and PC1 alternate. Which physical keys or buttons these are needs the hardware map, so it's **not yet confirmed**. The likeliest candidates are the front-panel buttons on the PC0/PC1/PA9 lines, but that's a guess.
4. If all four samples pass, it enters the updater. 0xD50 re-unlocks and sets ISPCON.ISPEN (0x5000C000 bit 0). 0xC84 clears the FIFOs. It waits delay(50000) (≈12.5 ms), then sets PA_DOUT[15:10] = 110011b (0x1E8; purpose unknown, maybe an LED or just parking the columns). 0xA54 starts USB, and it enters the SysEx loop at 0x22A, which never returns.

## 2. Handoff to APROM (0x64E)

```
0x64E  ISPCON (0x5000C000) &= ~0x2       ; BS = 0 -> boot from APROM
0x658  IPRSTC1 (0x50000008) |= 0x2       ; CPU_RST (CPU + FMC one-shot reset)
```
- REGWRPROT was already unlocked at 0xDF0, and nothing in LDROM re-locks it.
- It uses CPU_RST, **not** CHIP_RST, because a chip reset would reload BS from CBS and come back into LDROM. It doesn't use a direct jump either.
- **No valid-app check:** it doesn't check SP, the reset vector, a checksum or a flag. It reads nothing in data flash or SRAM. Because CBS=0, LDROM always runs first, so the updater can still be reached with a blank or bricked APROM, provided the key combination works.
- The updater has **no command that reboots into the app**. Command 5 is a no-op (0x47E `bx lr`). You leave update mode by power-cycling.
- Peripheral state left for the app: PLL at 48 MHz with HCLK=PLL (a CPU reset doesn't reset the clock controller, though the app reprograms it anyway), the GPIO modes above, and REGWRPROT unlocked. Whether CPU_RST re-locks REGWRPROT isn't verified, so the app should unlock it itself.

## 3. USB update mode

USB stack (0x684–0xC82): device registers at 0x40060000, USB clock enabled at APBCLK bit 27 (0xA54), USBD reset through IPRSTC2, DRVSE0 pulse, USB IRQ (NVIC bit 23). EP0 is control with 8-byte packets. The hardware maps EP2 to **0x81 IN (bulk 64)** and EP3 to **0x02 OUT (bulk 64)** (0x8A8).

**Descriptors** (0xCA4, served by GET_DESCRIPTOR at 0x7BE):
- Device (0xCD4): `12 01 10 01 00 00 00 08 67 0A 8F 13 00 01 01 02 00 01`. That gives USB 1.1, **VID 0x0A67, PID 0x138F**, bcdDevice 1.00, iManufacturer 1, iProduct 2, no serial number.
- Strings: 0 = 0x0409, 1 = "MEDELI", 2 = "Updater", 3 = "MIDI" (jack name).
- Config (0xCE6, 0x65 bytes): standard Audio Control interface (class 1/1) plus MIDI Streaming interface (1/3), with one embedded and one external IN/OUT jack pair. **Class-compliant USB-MIDI**, so ALSA and other host MIDI stacks see a normal MIDI port. There's no HID, DFU or vendor interface.

Data path: OUT packets are unpacked from USB-MIDI events into raw MIDI bytes by 0x920 (CIN 4/7 → 3 bytes, 6 → 2, 5 → 1, others dropped) and go into a 1280-byte RX ring (0x20000754). Replies are queued as raw bytes (0xC18) and repacked into CIN 4–7 events (0x544/0x5D8).

### SysEx protocol (parser 0x22A–0x2D2, handlers in the table at 0xE38)

All 32-bit values are sent as **4 × 7-bit groups, least significant first**: `b0 | b1<<7 | b2<<14 | b3<<21` (encoder 0x510, decoder 0x52C).

Request frame (offsets count from F0 = 0):
```
0     F0
1..8  00 20 2B 4D 49 44 49 7F      ; must match the table at 0xF54 (check at 0x4E8)
9     cmd
10    lenLo   11 lenHi             ; len = lenLo | lenHi<<7 = total bytes from F0 to the last
                                   ;   payload byte (the F7 is not counted)
12..  payload
      F7                           ; the command runs when F7 arrives
```
If len ≤ 12 (no payload), the command is ready once byte 11 is in. A status byte (≥0x80) other than F0/F7 resets the parser.

Status reply (0x4C0): `F0 00 20 2B 4D 49 44 49 7F <code> 00 00 F7`. The code is `04` OK, `05` bad block checksum, `06` final verify OK, or `07` final verify mismatch (table at 0xF60).

| cmd | handler | payload (offset from F0) | action |
|---|---|---|---|
| 1 | 0x2E6 | 12–15 addr (7-bit×4), 16 addr bits 31..28, 17–20 size | **Checksum query.** Sets state "mode 1". Replies `04`, then `F0 00 20 2B 4D 49 44 49 7F 02 04 00 s0 s1 s2 s3 F7`, where s is the 32-bit **byte sum** of flash[addr, addr+size), read through ISP READ (0x60A) as size/4 words. |
| 2 | 0x324 | as cmd 1, plus 21–24 the expected byte sum | **Start programming session.** Stores start, size and expected sum. Sets "mode 2". Replies `04`. |
| 3 | 0x390 | data pairs, then the last 4 bytes are the block sum | **Write block** (only in mode 2; otherwise ignored silently). Every pair `(lo7, hi1)` → `lo + (hi<<7)`. The decoded block sum must equal the trailing 32-bit value, or it replies `05`. It **page-erases** (ISPCMD 0x22) at the current address, retrying every 25 µs until it succeeds, then **programs 128 words = 512 bytes** (ISPCMD 0x21, retrying on failure) and replies `04`. The address advances by (len−4)/2. On the last block it byte-sums [start, start+size) and replies `06` or `07`. |
| 4 | 0x454 | none | Mode 1 only. Advances a 512-byte read window (addr/remaining). **Sends nothing.** Looks like an unfinished read-back feature. |
| 5 | 0x47E | none | No-op. |

**Caveat (verify before relying on it):** as disassembled, cmd 3 decodes pairs starting at **offset 0** of the message (0x3B0–0x3D2: `r5=0`, it reads `buf[4+2i], buf[5+2i]`), not at offset 12. The 12-byte header (offset 0 is the stale F0 slot) would then turn into the first 6 decoded bytes of every 512-byte page. The address step and the alignment only work out when (len−4)/2 = 512, so len = 1028. In that case each page would get 6 header-derived bytes and 506 payload bytes, which can't produce a working image. Either the Medeli host tool relies on some convention I haven't spotted, or the decode really is this way and I've misread an offset. Treat the stock updater as **documented but not validated**. Don't use it as the recovery path without testing on a sacrificial setup. SWD restore (`NOTES.md`) is still the recovery path.

## 4. Flash write paths and protection

- The ISP helpers are the only FMC users: read 0xD6A (ISPCMD 0x00), program 0xD8C (0x21), page erase 0xD9E (0x22), trigger and fail check 0xDAE (ISPTRG=1, checks and clears ISPCON.ISPFF bit 6). It doesn't poll ISPGO, relying on the CPU stall during ISP.
- **No software address check.** cmd 2 takes any 32-bit address, including bits 31..28. Protection is left to hardware: ISPCON only sets ISPEN, never **LDUEN (bit 5)** or **CFGUEN (bit 4)**, so LDROM and the CONFIG words can't be erased or written, and the ISP fails. When running from LDROM, APROM and data flash (0x1F000) are writable. A command aimed at LDROM or CONFIG would make the erase loop at 0x3E2 **retry forever** (hang until power-cycle). It can't brick the bootloader.

## 5. Does APROM request the bootloader?

No. The APROM literal pools have **no references to IPRSTC1 (0x50000008), AIRCR (0xE000ED0C) or any ISPCON.BS write**. Its FMC code (0x3468–0x3720) only does ISPEN and data-flash read/program/erase for the settings journal. The only GCR reset it touches is IPRSTC2 (0x5000000C, USB reset at 0x1638). The bootloader can only be entered with the key combination at power-up. There are no SysEx or flag paths.

## Notes for our own bootloader
- The handoff pattern to copy is: unlock → `ISPCON &= ~BS` → `IPRSTC1 |= CPU_RST`. With CBS=0 kept, our LDROM always gets control first, which is a good recovery story.
- Add what stock lacks: an APROM validity check (SP inside SRAM 0x20000000–0x20001000, reset vector inside APROM, ideally a CRC), a software-requested entry (for example a magic word in SRAM that survives CPU_RST, or a data-flash flag), and an explicit reboot command.
- The stock code shows that 48 MHz from the 12 MHz crystal through PLLCON=0xC22E works on this board (X1 is 12 MHz).
