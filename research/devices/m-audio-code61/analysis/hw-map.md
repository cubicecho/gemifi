# Code 49/61: hardware map from the stock app

Written 2026-11-02. This is static analysis of the Code 49 v1.2.1 app image (`firmware/app_v1.2.1_0x08002400.bin`). No hardware was touched. All addresses are flash addresses in that image. Longer quotes from the vendor code are kept in `private/hw-map-notes.md` (gitignored).

The Code 61 runs its own build of the same firmware. §9 re-checks every finding on the real Code 61 v1.2.2 app (`firmware/app_code61_v1.2.2_0x08002400.bin`): the pins, EEPROM layout, update path and clock are unchanged; only the key map and key count differ.

Confidence: **sure** = read directly from register setup; **likely** = the setup is clear and the purpose is inferred; **guess** = needs a board photo or a hardware test.

## 1. Summary

| | |
|---|---|
| MCU | STM32F103 **medium density**, 64-pin (e.g. STM32F103RB), or a GD32 clone. This corrects the README: there is no UART5 and no DMA2 use (§7) |
| Clock | 8 MHz HSE, PLL ×9 = 72 MHz (sure, §5) |
| Key matrix | Rows come from 3-to-8 decoders on PB3-PB7; columns are PB8-PB15 with pull-ups, active-low |
| Buttons, encoders | On the same matrix, rows 16-22. No panel button is on its own GPIO |
| LEDs + display | Two TM1628-class LED drivers, bit-banged on PC10/PC11 with strobes PA15/PC12 |
| Analog | ADC1, 9-rank scan with DMA. One 8:1 mux step (PC0-PC2) per frame |
| MIDI out | USART1 TX on PA9, 31250 baud |
| USB pull-up | PA8, high = D+ pull-up on |
| SWD | Left on. The app only turns JTAG off (`GPIO_Remap_SWJ_JTAGDisable`) |
| Free EEPROM pages | **0x0801EC00, 0x0801F000, 0x0801F400**: never read or written by the app (§3) |

## 2. Pin map

GPIO setup happens in three places: board init 0x08011144, ADC init 0x08002C4C and serial init 0x08012E60. All three use the StdPeriph `GPIO_Init` at 0x08011208. USB init is at 0x08013540. The APB2 clock enables are AFIO, GPIOA/B/C, ADC1 and USART1. APB1 enables USART2 and USB, and AHB enables DMA1. No GPIOD/E/F/G is ever touched.

| Pin | Mode set by the app | What | Confidence | Evidence |
|---|---|---|---|---|
| PA0 | analog | ADC ch0, rank 1: pads 1-8 via mux | sure (pads: likely) | 0x08002CB4, 0x080029B4 |
| PA1 | analog | ADC ch1, rank 2: pads 9-16 via mux | sure (pads: likely) | same |
| PA2 | AF push-pull | USART2 TX, 115200 baud, TX only. Purpose unknown (debug or a test port?). RX (PA3) isn't set up | sure (pin), unknown (use) | 0x08012E84, 0x08012EDE |
| PA4 | analog | ADC ch4, rank 3: a wheel (pitch or mod), 5-sample average | sure (pin), likely (wheel) | 0x08002868 |
| PA5 | analog | ADC ch5, rank 4: the other wheel | sure (pin), likely (wheel) | 0x080028AE |
| PA6 | analog | ADC ch6, rank 5: one fader, direct (fader slot 0) | sure | 0x080029EA |
| PA7 | analog | ADC ch7, rank 6: 8 faders via mux (fader slots 1-8) | sure | 0x08002A5C |
| PA8 | push-pull out | USB D+ pull-up switch, high = connected | sure | 0x0801357C, 0x080134FC, set to 1 in USB power-on 0x08011BAE |
| PA9 | AF push-pull | USART1 TX = 5-pin MIDI out, 31250 baud, DMA1 ch4 | sure | 0x08012E6A, 0x08012EAE |
| PA10 | input floating | USART1 RX (enabled, TX+RX mode). Probably no MIDI-in jack | sure (pin), guess (jack) | 0x08012E96 |
| PA11/12 | – | USB D−/D+ | sure | – |
| PA13/14 | – | SWDIO/SWCLK, left enabled | sure | remap 0x00300200 at 0x08011150 |
| PA15 | open-drain out | STB of LED driver A (14 bytes of display RAM) | likely | bit-band writes in 0x08012BC0 |
| PB0 | analog | ADC ch8, rank 7: unknown use (expression pedal?). 5-sample average, ±36 hysteresis | sure (pin), guess (use) | 0x080028F4 |
| PB1 | push-pull out | XY pad: X drive | likely | 0x08002C7C, 0x08002798 |
| PB2 | open-drain out | XY pad: Y drive | likely | 0x08002C92, 0x08002812 |
| PB3-PB5 | push-pull out | Matrix row address (3 bits into a decoder) | sure | ODR mask 0xFFC7 at 0x08011FC4, table 0x0801410C |
| PB6 | push-pull out | Decoder group select: 1 for rows 0-7 | sure | bit-band 0x08011FD6 |
| PB7 | push-pull out | Decoder group select: 1 for rows 8-15. Both low: rows 16-22 | sure | same |
| PB8-PB15 | input pull-up | Matrix columns, read as `~IDR >> 8` (active-low) | sure | 0x08011FFA-0x0801201C |
| PC0-PC2 | push-pull out | Mux select, the step number 0-7 | sure | 0x08002C52, 0x080027D0 |
| PC3 | analog | ADC ch13, rank 8: aftertouch sensor (window 0x4D0-0x900) | sure (pin), likely (use) | 0x0800293E, 0x080087F0 |
| PC4 | analog / out | XY pad: ADC ch14 (rank 9, odd steps), else drives Y high | likely | 0x0800277C, 0x080027F6 |
| PC5 | out / analog | XY pad: drives X low (odd steps), else ADC ch15 (rank 9) | likely | same |
| PC9 | input pull-up | Sustain jack. Debounced 10 scans; polarity learned at power-up | likely | 0x080120FC, 0x080122C2 |
| PC10 | open-drain out | LED driver CLK | likely | 0x08012BC0 |
| PC11 | open-drain out | LED driver DIO | likely | same |
| PC12 | open-drain out | STB of LED driver B (6 bytes) | likely | same |
| PC13 | input pull-up | Unknown. Debounced 10 scans. When it goes low, a 2 s timer starts; then pending settings are flushed to flash and PC15 goes low | sure (setup), unknown (use) | 0x080120B4, 0x08010622 |
| PC15 | push-pull out | Unknown, tied to the PC13 logic above | sure (setup), unknown (use) | 0x080120E8, 0x08010632 |

No mux enable line is driven. If the muxes have an /E pin, it's strapped.

### Key and button matrix

The scan is at 0x08011F90. It runs 23 rows. For each row it:

1. writes the row number into PB3-PB5 and the group into PB6/PB7;
2. switches PB8-PB15 to input pull-up (CRH = 0x88888888);
3. waits 30 loop passes;
4. reads `IDR & 0xFF00`;
5. switches PB8-PB15 back to push-pull outputs driven high (CRH = 0x33333333, ODR |= 0xFF00).

The stored value is inverted, so 1 = contact closed.

| Rows | Group (PB7, PB6) | Use | Debounce | RAM |
|---|---|---|---|---|
| 0-15 | 0-7: (0,1); 8-15: (1,0) | Keys: row 2k = first contact, row 2k+1 = second contact, same column | 1 scan | 0x20001308 |
| 16-20 | (0,0) | Buttons: 40 slots, slot = (row − 16) × 8 + column | 80 scans | 0x200001C1-C5 |
| 21-22 | (0,0) | Encoders 1-4 (row 21) and 5-8 (row 22): A = column 2n, B = column 2n+1 | 1 scan | 0x200001BA-BB |

"Column" c means PB(8+c).

**Keys.** The key timing code at 0x08012144 looks a key up as `map[(row / 2) × 8 + column]`, using the map at 0x08014C18. That map's key numbers count down from the start of the table. The Code 49 layout is below (key 0 = the lowest C, from the black-key table at 0x08014123):

| Row pair | Columns 0-7 |
|---|---|
| rows 0/1 | –, –, –, 48, 47, 46, 45, 44 |
| rows 2/3 | 43 … 36 |
| rows 4/5 | 35 … 28 |
| rows 6/7 | 27, 26, 25, 24, –, –, –, – |
| rows 8/9 | 23 … 16 |
| rows 10/11 | 15 … 8 |
| rows 12/13 | 7 … 0 |
| rows 14/15 | all unused |

That leaves 15 slots free (3 + 4 + 8). The Code 61's 12 extra keys must sit in some of them, but which ones can't be read from the 49 image.

**Encoders.** The decoder is at 0x080122F6. On a 0→1 change of A it counts a step, and B gives the direction (0x81 / 0x82). That is ×1 decoding.

**Buttons.** Slots 0 and 1 (row 16, PB8 and PB9) are a special pair. The app checks "both held" and "one held for 900 ms" (0x0800899E, 0x08002E40), which fits octave −/+. That's a **guess**: we haven't decoded which physical button each slot is. Two more combos are checked in 0x08013BBC: slots 15+16 held alone, and slots 17-19 held alone, both for 300+ ticks. They look like hidden test or demo modes.

### Analog scan

ADC init is at 0x08002C4C. ADC1 runs a 9-rank regular scan with DMA1 ch1 into 0x200002A0, started by software. Ranks 1-8 use sample time 41.5 cycles; rank 9 uses 71.5. Two frames are summed per mux step (0x08002708), and then the mux steps on.

| Rank | Channel | Pin | Per step | Processing (0x08002854) |
|---|---|---|---|---|
| 1 | 0 | PA0 | mux | pads, slot `step` → pad table 0x08014C08 |
| 2 | 1 | PA1 | mux | pads, slot `step + 8` → same table |
| 3 | 4 | PA4 | direct | wheel, 5-sample average, ±10 hysteresis |
| 4 | 5 | PA5 | direct | wheel, same |
| 5 | 6 | PA6 | direct | fader slot 0, 4-sample average, 32/160 deadband |
| 6 | 7 | PA7 | mux | fader slots 1-8 |
| 7 | 8 | PB0 | direct | unknown, 5-sample average, ±36 hysteresis |
| 8 | 13 | PC3 | direct | aftertouch, 4-sample average |
| 9 | 14 / 15 | PC4 / PC5 | alternating | XY pad X / Y, 3-sample average, ±34 |

That makes 16 pads, 9 faders, 2 wheels, aftertouch, the XY pad and one unknown input. There are no analog knobs: the 8 encoders are digital, on the matrix.

### LEDs and display

There are two serial LED drivers, bit-banged LSB first: data is set while CLK is low and clocked in on the rising edge. The commands are 0x44 (fixed address), 0xC0+addr and 0x88|brightness (0x08012BC0, 0x08012C80, 0x08012CEC, refreshed from 0x080116D4).

- Chip A (STB PA15) gets 14 RAM bytes. That's the TM1628 size.
- Chip B (STB PC12) gets 6 bytes.

Together they drive every LED and the 6-character display (text is written at 0x08011524, positions 0x40-0x45). No key-scan read-back from the drivers was found.

## 3. Settings EEPROM: which pages are used

There is one reader, 0x08010E1C (offset, dst, len), and one page writer, 0x08010E4C (offset). The writer always writes a whole 1 KB page from the buffer at 0x20001318. Offsets below 0x4800 are relative to 0x0801B400. The app never forms an address in 0x0801B400-0x0801FFFF any other way: the only literals in that range are 0x0801B400 and 0x0801FC00.

All 17 writer calls and 16 reader calls use fixed offsets:

| Page | Offset | Contents | Writers / readers |
|---|---|---|---|
| 0x0801B400 | 0x0000 | 0x35-byte global block (calibration and global settings), read at boot | W 0x080107AC, 0x08010CC6, deferred 0x080108A4; R 0x08010AC0 |
| 0x0801B800 | 0x0400 | 15-byte settings at 0x20003C97 (octave, transpose, curves, acceleration ...) | W 0x080107D6, 0x08010D08, deferred 0x080108AA; R 0x08010CD6 |
| 0x0801BC00-0x0801E800 | 0x0800 + n×0x400, n = 0..11 | 12 presets, 0x3FF bytes each (defaults in flash at 0x080155CF + n×0x3FF) | W 0x08010820, 0x08010DC6, SysEx 0x0800A374/0x0800A720/0x0800AA0C, deferred 0x080108B2; R 0x080106A6, 0x08010D2A, SysEx 0x0800A342/… |
| 0x0801EC00 | 0x3800 | **never used** | – |
| 0x0801F000 | 0x3C00 | **never used** | – |
| 0x0801F400 | 0x4000 | **never used** | – |
| 0x0801F800 | 0x4400 | 12 preset names, 12 bytes each at n×0x50 (defaults at 0x080185C3) | W 0x0801084E, 0x08010DF0, 0x080108FE, SysEx 0x0800A39C/0x0800A742/0x0800A9EE; R 0x0801068A, 0x08010D40, 0x080108F0, … |
| 0x0801FC00 | absolute | update-request flag (custom-firmware-path.md) | W 0x080074DC |

Every preset index is checked with `n < 12` before use (0x08010676, 0x08010692, 0x08010882, the loops at 0x08010828 and 0x08010DCE). No pages rotate and nothing is wear-levelled: each record has one fixed page and is rewritten in place. Writes are deferred (0x080108B8, flushed by 0x080109F0) and skipped when the page already matches.

**Answer:** gemifi can use **0x0801EC00 and 0x0801F000** (0x0801F400 is a third spare). The stock app never reads, writes or erases them, so a user who goes back to stock keeps their presets, and gemifi's config survives. Two things are unknown:

- **The bootloader.** We have no dump of it. It erases only what the update stream tells it to (0x08002400-0x0801B3FF), but it could have its own use for these pages. Check them once with a read-out before trusting them.
- **Stale contents.** The pages may hold factory leftovers (a production test, or an older firmware layout). gemifi must check its own magic and CRC, and never assume they're blank (0xFF).
- **A bad SysEx can reach them.** The editor's "set one parameter" message (command 0x67, Code 49 0x0800A934) takes the preset number from the message and writes page 0x800 + (n − 1) × 0x400 without checking n ≤ 12. Every other path checks it. So under the stock app, a message with preset 13 or 14 rewrites 0x0801EC00 or 0x0801F000 (read, change one byte, write back). Only a buggy or hostile host sends that, and it can't happen while gemifi runs. The magic + CRC check covers it. Same in the Code 61 build (§9).

**Per-unit calibration** can only be in the 0x35-byte block at 0x0801B400, since that's the only per-unit-looking record. gemifi should read that page and never write it. Which bytes are calibration hasn't been decoded.

## 4. Recovery combo

No panel button is on a plain GPIO. The only direct digital inputs are PC9 (sustain jack) and PC13 (unknown).

Reading one matrix row needs only RCC and GPIO, with no mux, ADC or timer, so it's still fine for a reset-handler check:

1. Enable the AFIO and GPIOB clocks and do the same remap the app does (`GPIO_Remap_SWJ_JTAGDisable`: AFIO_MAPR SWJ_CFG = 010). At reset PB3 is JTDO and PB4 is NJTRST, so they can't drive the row address until JTAG is off. SWD stays on.
2. Set PB3-PB7 as push-pull outputs, all low. That selects row 16.
3. Set PB8-PB15 as input pull-up (CRH = 0x88888888, ODR bits 8-15 = 1).
4. Wait about 5 µs, then read IDR. A pressed button reads 0.

| Candidate | Port/pin | Active level | Confidence |
|---|---|---|---|
| Button slot 0 (octave − ?) | row 16 (PB3-PB7 = 0), column PB8 | low | pins sure, label guess |
| Button slot 1 (octave + ?) | row 16, column PB9 | low | pins sure, label guess |
| Sustain pedal | PC9, input pull-up | depends on the pedal (the app learns it) | likely |

Suggested combo: slots 0 + 1 held at power-up. Confirm on hardware which buttons they are before relying on it.

## 5. Clock

- RCC setup is at 0x08011C14: RCC_DeInit, HSE on, flash latency 2, AHB /1, APB2 /1, APB1 /2, then PLL source HSE ×9 (`0x1C0000`), and SYSCLK = PLL.
- USB clock = PLL / 1.5 (0x08013546, `RCC_USBCLKConfig(0)`), which gives 48 MHz only if the PLL runs at 72 MHz.
- `HSE_VALUE` = 8 000 000 (literal 0x007A1200 at 0x08011D50, in `RCC_GetClocksFreq`).
- The USART BRR values come from that frequency (31250 and 115200 baud, set through `USART_Init`).

So HSE = **8 MHz** (sure): USB couldn't enumerate with any other crystal at ×9 / 1.5. SysTick: see mushy-feel.md (one tick = 200 µs).

## 6. Code 49 vs Code 61

The firmware has a key-count constant rather than a loop bound tied to the matrix. The matrix loop always covers rows 0-15 (`cmp r2,#0x10` at 0x0801226A), and the key map limits which slots count as keys.

| Item | Code 49 value | Where |
|---|---|---|
| Key map (scan slot → key) | 49 keys, 64 slots | 0x08014C18 |
| "Valid key" check | `cmp r1,#0x31` (≤ 49) | 0x08012190 |
| Other `#0x31` compares (probably the key count: velocity, SysEx, display) | 49 | 0x08006BF2, 0x080099AA, 0x0800A31E, 0x0800A5D8, 0x0800B83A, 0x08011514 (unchecked) |
| Black-key table | already 61 entries long (C to C) | 0x08014123 |
| Default octave/transpose | octave 5, transpose 12: key 0 = note 36 | 0x080155C0 (+5, +7), formula at 0x08009818 |

So a Code 61 build should differ in the key map, the 0x31 → 0x3D constants and maybe the default note range. Everything else (pins, analog, EEPROM layout) looks shared. §9 confirms this on the Code 61 image. Not all the `#0x31` compares above are the key count: three of them (0x08006BF2, 0x0800A31E, 0x0800A5D8) are the M-Audio "Code" family byte in SysEx, and they stay 0x31 on the Code 61.

## 7. Corrections to earlier notes

- **No UART5.** 0x40005000 is used only as `0x40005000 + 0xC00 + 4n`, which is the USB endpoint registers (EPnR at 0x40005C00), e.g. 0x08007D5A.
- **No DMA2.** 0x40020400 appears only inside the StdPeriph DMA flag helpers (0x0800B4D0-0x0800B610). The channels actually configured are DMA1 ch1 (ADC) and ch4 (USART1 TX).
- That removes the "high-density" evidence. A medium-density part with 1 KB pages, 128 KB of flash and a 64-pin package (PC0-PC15 are used) fits everything: **STM32F103RB**, or a GD32F103RB-class clone. That also settles the "1 KB pages on a high-density part?" puzzle in custom-firmware-path.md.
- SWD stays enabled while the stock app runs. Only JTAG is turned off, which frees PA15, PB3 and PB4.

## 8. Needs hardware

Updated 2026-11-03, after gemifi ran on a Code 61 (`devices/m-audio-code61.yaml` has the details).

1. ~~Chip marking~~: STM32F103RBT6 (chip marking, SWD IDCODE 0x20036410).
2. ~~The Code 61 key map and key count~~: done from the Code 61 app (§9), and every key checked on hardware.
3. ~~Which physical button is each of the 40 button slots~~: labelled on hardware (the YAML's `buttons:`). Slots 0/1 are Down / Up, not octave −/+.
4. Which fader is slot 0 (PA6), and which order slots 1-8 follow: still open (PA6 is taken to be the master fader).
5. ~~Which wheel is PA4 and which is PA5~~: PA4 is pitch, PA5 mod. What PB0 (ch8) is for: still open (most likely the expression pedal jack).
6. What PC13 and PC15 do: still open. When PC13 goes low, the stock app flushes pending settings 2 s later and then drives PC15 low, which looks like a power-loss warning. ~~What USART2 (PA2) talks to~~: the pad LED board (core/README.md "RGB serial LEDs").
7. ~~The settings pages~~: gemifi's settings store works in 0x0801EC00/0x0801F000 on hardware (settings and presets save). A power cut during a write is still untested (#13).
8. The mux part number: still open; the 4 us settle works. Whether there's a MIDI-in jack on PA10: see the DIN MIDI work (PR #50).

## 9. Code 61 v1.2.2 check

Checked 2026-11-02 on `firmware/app_code61_v1.2.2_0x08002400.bin`, statically. The two images are only about 43% byte-identical, but almost all of that is code moving. Lined up function by function, the Code 61 code is the same as the Code 49 code apart from the key count, key arrays, SysEx model byte and reply version. In the 0x08010000+ area most functions sit 0xD8 higher; code below 0x0800A000 mostly keeps its address. The image ends exactly at 0x08018800 (its scatter-load table says so), so it fills the 0x16400-byte update image with nothing to spare. It is not truncated.

Status: **same** = same code and values, at the Code 61 address given; **changed** = new value given.

| Finding | Code 49 v1.2.1 | Code 61 v1.2.2 | Status |
|---|---|---|---|
| EEPROM reader / page writer | 0x08010E1C / 0x08010E4C | 0x08010EF4 / 0x08010F24, 16 and 18 calls (same as Code 49) | same |
| Flash erase / program | called only from the writer | 0x080110DC / 0x08011170, each called only from the writer 0x08010F24 | same |
| Literals in 0x0801B400-0x0801FFFF | 0x0801B400, 0x0801FC00 | same two (0x08010F20, 0x08010FE4, 0x08007514) | same |
| Global block 0x0801B400 (0x35 bytes, calibration) | R 0x08010AC0 | R 0x08010B98, W 0x08010888 / 0x08010D9E | same |
| Settings 0x0801B800 (15 bytes) | 0x0400 | W 0x080108B2 / 0x08010DE0, R 0x08010DAE | same |
| 12 presets 0x0801BC00-0x0801E800 | n < 12 checks | still 12; checks at 0x0801095E, 0x08010E7E, 0x08010EA6, 0x08010EC2; SysEx store checks n < 13 at 0x0800A31E, 0x0800A6CA | same |
| Preset names 0x0801F800 | 0x4400 | 0x4400 (0x08010766, 0x0801092A, 0x080109DA, SysEx 0x0800A38C/0x0800A73C/0x0800A9EA) | same |
| 0x0801EC00, 0x0801F000, 0x0801F400 | never used | never used by any normal path. Only the unchecked SysEx 0x67 write (0x0800A930, page write at 0x0800AA08) can reach them, as in Code 49 (§3) | same |
| Defaults (preset/global tables) | 0x080155AC-0x080155CF, names 0x080185C3 | 0x08015684-0x080156A7, names 0x0801869B; contents byte-identical | same |
| Update request handler | 0x0800748C | 0x0800748C: 12-byte start message at 0x08014243 (same bytes) | same |
| Flag magic → 0x0801FC00 | `55 5A A5 AA 59 95 3A A3` | same bytes at 0x0801423B, page write at 0x080074DC | same |
| Reply | F0 15 F7 | F0 15 F7 (0x080074E6-0x080074FC), then return. No reset (the only AIRCR write, 0x08011AFC, is the NVIC priority grouping) | same |
| VTOR | 0x08002400 | `NVIC_SetVectorTable(0x08000000, 0x2400)` at 0x08011A74 | same |
| Clock | HSE 8 MHz × 9 | RCC setup 0x08011CF2-0x08011D2E, HSE_VALUE literal at 0x08011E28 | same |
| IWDG | PR 3, RLR 0x4E2 (~1 s) | 0x08011530, called from main 0x08013E84 | same |
| SWJ remap (JTAG off, SWD on) | 0x00300200 | 0x0801122A | same |
| Board GPIO init (PB3-PB7 out, PB8-PB15 IPU, PC9, PC13, PC15, PC10-12, PA15) | 0x08011144 | 0x0801121C, identical apart from addresses | same |
| USB init, PA8 pull-up, active high | 0x08013540, 0x080134FC | 0x08013618, 0x080135D4, USB power-on 0x08011C86 | same |
| Matrix scan, 23 rows, row table, delays, debounce | 0x08011F90, table 0x0801410C | 0x08012068, table 0x080141E4 (same 23 entries) | same |
| Buttons rows 16-20, encoders rows 21-22 | as §2 | same code (0x0801234C onwards), same row usage | same |
| Octave pair (slots 0/1), hidden combos | 0x0800899E, 0x08002E40, 0x08013BBC | 0x0800899E, 0x08002E40, 0x08013C94 | same |
| Key rows | 0-15 (row pair 14/15 unused) | 0-15, row pair 14/15 now used | changed |
| Key count / valid-key check | `cmp #0x31` at 0x08012190 | `cmp #0x3D` (61) at 0x08012268; also loops at 0x0800999A, 0x0800B83A, 0x080115EC | changed |
| Key map | 0x08014C18 | 0x08014CF0 (table below) | changed |
| Black-key table | 0x08014123, 61 entries | 0x080141FB, identical | same |
| Note offset | key 0 = note 36 | key 0 = note 36 (octave 5, transpose 12, formula at 0x08009818). 61 keys = notes 36-96 | same |
| SysEx model byte | 0x05 | 0x06 (identity reply `F0 7E id 06 02 00 01 05 31 06 …` at 0x0800A2AA; requests must carry 0x06, 0x0800A316 / 0x0800A5CE). Family byte 0x31 unchanged | changed |
| ADC init, 9 ranks, mux PC0-PC2, XY pad | 0x08002C4C, 0x08002700 | same addresses, same code; pad slot table 0x08014CE0, identical | same |
| USART1 MIDI out PA9, USART2 PA2 | 0x08012E60 | 0x08012F38 | same |
| LED drivers (PC10/PC11, STB PA15/PC12) | 0x08012BC0, refresh 0x080116D4 | 0x08012C98, refresh 0x080117AC | same |
| Sustain PC9, PC13/PC15 logic | 0x080120B4, 0x08010622 | 0x0801218C, 0x080106FE | same |

### Code 61 key map (0x08014CF0)

| Row pair | Columns 0-7 |
|---|---|
| rows 0/1 | 55 … 48 |
| rows 2/3 | 47 … 40 |
| rows 4/5 | 39 … 32 |
| rows 6/7 | 31 … 24 |
| rows 8/9 | 23 … 16 |
| rows 10/11 | 15 … 8 |
| rows 12/13 | 7 … 0 |
| rows 14/15 | –, –, –, 60, 59, 58, 57, 56 |

61 of the 64 key slots are used, all in rows 0-15. The keys don't spill into rows 16+, so the button and encoder rows (16-22) and the recovery combo (row 16, PB8 + PB9) are unchanged.

### What this means for gemifi

- **Config pages 0x0801EC00 / 0x0801F000:** still safe. The Code 61 app reads, writes or erases them only through the unchecked SysEx 0x67 path. Keep the magic + CRC check. The bootloader is still not dumped (§3).
- **Recovery combo:** unchanged: drive PB3-PB7 low (row 16), pull up PB8-PB15, read PB8 and PB9 active-low. Which physical buttons those are is still a guess (octave −/+).
- **USB pull-up PA8 active high, VTOR 0x08002400, HSE 8 MHz:** unchanged.
