# Keystation Mini 32 MK3: hardware map from the stock firmware

Source: `backup/aprom-read1.bin` (NUC120LC1BN APROM, base 0x0). This is static analysis only, with no hardware access.
Listing tools: `analysis/scripts/dis.py` (capstone linear Thumb sweep, base 0) and `analysis/scripts/rng.sh`.
The routine listings are kept locally in `analysis/excerpts-hw.txt` (gitignored: they are copied from the stock firmware).
Register meanings were checked against the NUC100/120 TRM V2.02 and the datasheet V2.03 pin diagram (Fig. 3-6, Low Density LQFP48).

Confidence levels: **H** means it is read directly from register writes. **M** means it is inferred from code behaviour. **L** means it is a guess.

---

## 0. Firmware layout (for orientation)

| Addr | What |
|---|---|
| 0x44F5 | Reset: `__main` (scatter-load: zero 0x200004A8..0x200009B7, copy 0xA8 bytes 0x4544→0x20000400) then `main` |
| 0x3B2C | `main()` |
| 0x4058 | clock init |
| 0x2BF8 | GPIO init (0x2CC4), USB init (0x162A), ADC init (0x1DC8), then the factory-fixture loopback test |
| 0x3468 | FMC: ISPCON.ISPEN=1 (settings journal) |
| 0x236C | ADC init again + matrix-GPIO init again (0x1EAE) + key-state init |
| 0x40AC | Timer0 init + NVIC enable |
| 0x3316 | PWM/LED init |
| 0x40E9 | TMR0 IRQ (vector 24 = IRQ8) |
| 0x16B3 | USBD IRQ (vector 39 = IRQ23) |
| 0x43FD | HardFault: semihosting-BKPT skipper (0xBEAB); NMI uses the same handler |
| 0x440F | default handler (infinite loop) |

---

## 1. Clock setup (0x4058; **H**)

```
REGWRPROT = 0x59,0x16,0x88        ; unlock (never re-locked afterwards)
PWRCON   |= 0x01 (XTL12M_EN) | 0x04 (OSC22M_EN)
PLLCON    = 0x0000C22E            ; PLL_SRC=0 (XT1), FB_DV=46, IN_DV=1, OUT_DV=3, PD=0
delay(5000)                       ; SysTick busy-wait, no CLKSTATUS polling
CLKSEL0.HCLK_S  = 010 (PLL)
delay(5000); delay(50000)
CLKSEL0.STCLK_S = 010 (XT1/2)     ; irrelevant: SysTick CTRL=5 uses the core clock
```
- FOUT = FIN × (46+2) / (1+2) / 4 = **FIN × 4**. 0xC22E is the TRM's own example value, "FIN = 12 MHz → 48 MHz".
- CLKDIV is never written, so HCLK_N=0 and USB_N=0. **HCLK = 48 MHz** and the **USB clock = PLL = 48 MHz**.
- USB FS needs exactly 48 MHz, so the **crystal X1 is 12 MHz**. That is also the only value that keeps the PLL VCO (FIN×16) inside its 100–200 MHz window.
- Timer0 and PWM run directly from the 12 MHz crystal (see §4).
- Quirk: `delay(n)` loads SysTick with n×12 core cycles, so it assumes a 12 MHz core. At 48 MHz the delays are 4× shorter than their nominal µs. For example, the "500 ms" boot wait in main is really about 125 ms.

Replacement firmware: enable XT1 and wait for CLKSTATUS.XTL12M_STB. Then set PLLCON = 0xC22E and wait for PLL_STB. Then set HCLK_S=PLL. Leave USB_N=0.

---

## 2. Pin table

LQFP48 pin numbers come from the datasheet Fig. 3-6. PMD values: 00 = input (no pull), 01 = push-pull output.
NUC120 inputs have **no internal pull resistors**. Only quasi-bidirectional mode (11) has a weak pull-up, and the firmware never uses it. Every input therefore relies on external pulls, which are the AR1–AR6 arrays.

| Port pin | LQFP48 | Mode set by FW | MFP | Role | Conf. | Code |
|---|---|---|---|---|---|---|
| PA.0 | 32 | input, OFFD bit16 set (digital path off) | GPA_MFP[0]=1 → **ADC0** | **Volume knob** (pot wiper) | H | 0x1DCA–0x1DDE |
| PA.1 | 33 | input | GPIO | **Button 0 = Sustain** (active-low) | H (pin) / H (function) | 0x2CC4 PMD; read 0x21F6; dispatch 0x15C |
| PA.2 | 34 | input | GPIO | **Button 1 = Edit** (active-low) | H / H | dispatch 0x178 |
| PA.3 | 35 | input | GPIO | **Button 2 = Octave −** (auto-repeat) | H / H | dispatch 0x1A6, 0x9FA |
| PA.4 | 36 | input | GPIO | **Button 3 = Octave +** (auto-repeat) | H / H | dispatch 0x1BA |
| PA.5 | 37 | input | GPIO | **Button 4 = Pitch-bend down** | H / H | dispatch 0x1CA → 0x2F58(0): state 3 = decrement |
| PA.6 | 38 | input | GPIO | **Button 5 = Pitch-bend up** | H / H | dispatch 0x230 → 0x2F58(1): state 1 = increment |
| PA.7 | 39 | input | GPIO | **Button 6 = Modulation** (CC1 ramp) | H / H | dispatch 0x1FC → 0x2FF2 |
| PA.8 | 7 | output, init low | GPIO | **Matrix drive line D0** | H | 0x2D3C, 0x1FD0 |
| PA.9 | 6 | output, init low | GPIO | **Matrix drive line D1** | H | 0x2D48, 0x1FD0 |
| PA.10 | 5 | output, init high | GPIO | **Octave− LED, second colour** (static, active-low) | H (mode) / M (role) | 0x32EC; 0x3374–0x33BC |
| PA.11 | 4 | output, init high | GPIO | **Octave+ LED, second colour** (static, active-low) | H / M | 0x32FC; 0x33C6–0x3418 |
| PA.12 | 28 | output → GPA_MFP[12]=1 **PWM0** | PWM0 | **Sustain LED** (active-low, PWM-dimmed) | H / M | 0x31D0; 0x9xx calls led_set(0,·) |
| PA.13 | 27 | output → GPA_MFP[13]=1 **PWM1** | PWM1 | **Edit LED** | H / M | 0x1A0 led_set(1,1) on entering edit mode |
| PA.14 | 26 | output → GPA_MFP[14]=1 **PWM2** | PWM2 | **Octave− LED, first colour** | H / M | 0xCB4 |
| PA.15 | 25 | output → GPA_MFP[15]=1 **PWM3** (ALT_MFP untouched, so not I2SMCLK) | PWM3 | **Octave+ LED, first colour** | H / M | 0xCB4 |
| PB.0 | 17 | input | GPIO (RXD0 unused) | **Matrix sense S0** | H | 0x2CC8, 0x1FDA |
| PB.1 | 18 | input | GPIO (TXD0 unused) | **Matrix sense S1** | H | " |
| PB.2 | 19 | input | GPIO | **Matrix sense S2** | H | " |
| PB.3 | 20 | input | GPIO | **Matrix sense S3** | H | " |
| PB.4 | 8 | input | GPIO (RXD1 unused) | **Matrix sense S4** | H | " |
| PB.5 | 9 | input | GPIO (TXD1 unused) | **Matrix sense S5** | H | " |
| PB.8 | 48 | input | GPIO | **Matrix sense S6** | H | 0x2CFA; bit8→bit6 at 0x1FDE |
| PB.12 | 1 | input | GPIO (CLKO unused) | **Matrix sense S7** | H | 0x2D02; bit12→bit7 at 0x1FE6 |
| PB.15 | 43 | not touched (reset = input) | — | **unused** (only free GPIO; INT1) | H | — |
| PC.0 | 24 | output, init low | GPIO | **Matrix drive line D2** | H | 0x2D56 |
| PC.1 | 23 | output, init low | GPIO | **Matrix drive line D3** | H | " |
| PC.2 | 22 | output, init low | GPIO | **Matrix drive line D4** | H | " |
| PC.3 | 21 | output, init low | GPIO | **Matrix drive line D5** | H | " |
| PC.6 | 42 | output, init low | GPIO | **Matrix drive line D6** | H | 0x2D86 |
| PC.7 | 41 | output, init low | GPIO | **Matrix drive line D7** | H | 0x2D92 |
| D+ / D− / VBUS / VDD33 | 16 / 15 / 13 / 14 | — | USB PHY | USB device (VBUS sensed via FLDET) | H | 0x162A |
| XT1_IN / XT1_OUT | 45 / 44 | — | — | 12 MHz crystal | H | §1 |
| X32I / X32O | 3 / 2 | — | — | not used (XTL32K never enabled) | H | — |
| ICE_DAT / ICE_CK | 29 / 30 | — | — | SWD | — | — |

That accounts for all 31 GPIOs of the LQFP48 part. **Every one except PB.15 is used.**

### GPIO init values (what a replacement must reproduce)
```
PA_PMD = PA0..7 input (00); PA8..15 output (01)   -> 0x5555_0000
PA_OFFD |= (1<<16)                                 ; PA0 digital input off (ADC)
PA_DOUT : PA8,PA9 = 0 ; PA10..PA15 = 1             ; LEDs off, drive lines idle low
PB_PMD  = PB0..5, PB8, PB12 input (00)             ; PB15 untouched
PC_PMD  = PC0..3, PC6, PC7 output (01)             -> 0x0000_5055
PC_DOUT : PC0..3, PC6, PC7 = 0
GPA_MFP = (1<<0) | (1<<12)|(1<<13)|(1<<14)|(1<<15) ; ADC0 + PWM0..3
```
The stock FW programs the MFP for PWM *after* GPIO init (0x3316). Until then PA12–15 are GPIO outputs driven high, which means LEDs off.

---

## 3. Key matrix

**Shape: 8 drive lines × 8 sense lines = 64 contacts = 32 keys × 2 contacts.** The buttons are **not** in the matrix. They are 7 direct GPIOs on PA1..PA7.

- **Drive lines (outputs, one-hot active-HIGH).** D0..D7 = PA8, PA9, PC0, PC1, PC2, PC3, PC6, PC7. All are low at idle. To scan line n, the firmware sets that line's DOUT bit to 1, reads, then clears it (0x1FC0). The others stay actively driven low (push-pull, not tri-stated).
- **Sense lines (inputs, active-HIGH).** The sense byte is bit0..5 = PB0..PB5, bit6 = PB8, bit7 = PB12. It is read from **PB_PIN** (0x50004050) **immediately** after the drive line goes high, with no settling delay (only a few cycles at 48 MHz). A closed contact reads **1**, so the sense lines must have external **pull-downs**. The previous-state RAM starts at 0, which is consistent with idle = 0. The board presumably has one diode per contact.
- **Key ↔ matrix mapping (0x2050).** Key index k = 0..31, where key 0 is the lowest key (C).
  `drive line = k mod 8`, `sense pair p = k div 8`, `contacts = sense bits (2p, 2p+1)`.
  - Pairs: p0 = PB0/PB1 → keys 0–7, p1 = PB2/PB3 → keys 8–15, p2 = PB4/PB5 → keys 16–23, p3 = PB8/PB12 → keys 24–31.
  - Contact order: for **keys 0–15** the **first** contact (timer start) is the **odd** bit (PB1, PB3) and the second contact (note-on) is the even bit. For **keys 16–31** the two are **swapped** in software (0x20C6–0x20D6): first = even bit (PB4, PB8), second = odd bit (PB5, PB12).
- **Scan rate.** All 8 lines are scanned in every Timer0 interrupt, i.e. every **125 µs** (8 kHz), inside the ISR (0x2378). The ISR XORs each line's byte with its previous value. For each changed pair it calls the per-key state machine with the pair's 2-bit value.
- **Velocity state machine (0x20C0).** The state is kept per key at 0x200007A8 + 8k (state byte + 32-bit timestamp):
  - idle → first contact only → **TIMING**, timestamp = tick counter (0x20000974, incremented each tick).
  - TIMING → second contact closes → **ON**: Δ = ticks since the timestamp, clamped to 1023. The firmware sends the internal event (key, speed). The key goes back to idle without a note if both contacts open while it is TIMING.
  - ON/HELD → both contacts open → note-off (key, 0). It does not wait for the first contact to open; release is detected when *both* are open.
- **Timer used for velocity:** Timer0 (§4), at a resolution of 125 µs over a 0–127.9 ms window.

### Buttons (0x21F4, every 80 ticks = 10 ms)
- The firmware reads `PA_PIN >> 1` (7 bits, PA1..PA7). Idle is **1**, because the initial "previous" value in RAM is 0x7F. The buttons are therefore **active-low with external pull-ups**.
- On each edge the button's state advances (pressed → held → released). Auto-repeat is enabled only for buttons 2 and 3 (octave): it starts after 51×10 ms and then repeats every 6×10 ms.
- Internal events are 0xB0+idx with value 1 = press, 0 = release, 2 = repeat. Octave− and Octave+ pressed together produces 0xBC, which resets the octave to 0.
- Power-up combinations seen in `main`:
  - Oct− and Oct+ held (`PA_PIN[7:1] == 0x73`, 0x23C8) → **factory reset** of the settings (channel 0, knob CC 7, …).
  - Sustain + Edit + Oct− held → `main()` returns, so the firmware parks in the semihosting `exit` loop. That is possibly a "stop" mode for ISP; worth knowing for the LDROM work.
- Sustain: settings param 14 = 0 → **latching** toggle on press. Param 14 ≠ 0 → momentary 127/0. The momentary behaviour you observed implies param 14 ≠ 0 on your unit.

---

## 4. Peripherals used

| Peripheral | Config | Conf. | Code |
|---|---|---|---|
| **Timer0** | CLKSEL1.TMR0_S=000 (12 MHz XT1), prescale 0, TCMPR=1500, TCSR=0x6800_0000 (CEN, IE, periodic) → **8 kHz / 125 µs tick**. The ISR clears TISR, scans the matrix, handles ADC timing (start at count 7, read at 21, period 28 ticks = 3.5 ms) and buttons (every 80 ticks), and bumps a ramp counter | H | 0x40AC, 0x40E8, 0x2378 |
| **ADC** | ch0 only (PA0), ADCHER=1. ADCR: ADMD=10 (single-cycle scan), ADEN, no interrupt (polled ADSR.ADF). The 12-bit result >> 4 gives 8 bits. Then a 4-sample moving average, ±1 hysteresis, and clamp (<2→0, ≥255→255). This becomes event 0x80 → CC (the CC number is a setting) | H | 0x1DC8, 0x1E20, 0x1E6A |
| ADC clock | CLKSEL1.ADC_S = **10b** (reserved in TRM V2.02; HCLK in later revisions?), ADC_N=0, APBCLK.ADC_EN. If that means HCLK it is 48 MHz, well above the 16 MHz spec. **Don't copy it.** Use ADC_S=00 (12 MHz XT1) | H (value) / L (meaning) | 0x1DE0–0x1DFE |
| **PWM A (PWM0–3)** | APBCLK PWM01_EN+PWM23_EN; CLKSEL1 PWM01_S=PWM23_S=00 (12 MHz); PPR CP01=CP23=0x5B (÷92); CSR all ÷2; auto-reload; CNR=0xFF → **≈255 Hz**. CMR=0xE3 at init (high 89%); POE bits 0–3 on; no inversion | H | 0x31CE |
| USB device | APBCLK.USBD_EN, IPRSTC2.USBD_RST pulse, ATTR=0x7D0, DRVSE0 pulse, INTEN.FLDET_IE (others enabled later), NVIC IPR5 IRQ23 priority 2 | H | 0x162A |
| FMC/ISP | ISPCON.ISPEN=1; settings journal at 0x1F000 | H | 0x3468 |
| SysTick | busy-wait delays only (CTRL=5, no interrupt) | H | 0x4040 |
| **NVIC** | ISER = bit 8 (TMR0) and bit 23 (USBD) only | H | 0x40DE, 0x1698 |
| **WDT** | **not used** (no access to 0x40004000) | H | — |
| **UART** | **not used**. No UART base address appears anywhere. UART0 pins PB0/PB1 and UART1 pins PB4/PB5 are **matrix sense lines** | H | — |
| I2C/SPI/I2S/PDMA | not used | H | — |

**Debug console:** there is no spare UART pin. Your options are SWD (semihosting or an RTT-style RAM buffer), or bit-banged TX on **PB.15 (pin 43)**, the only unused GPIO (if it is routed to a pad). A key matrix line could also be borrowed during bring-up.

---

## 5. Velocity tables: confirmed, but there are two stages

1. **Time → "speed" (0..255).** Index = Δticks (0..1023, 125 µs each) (0x2174):
   - **0x0E18–0x1217**: white keys (1024 bytes, 255 at Δ≤4, down to 1 at Δ≈958).
   - **0x1218–0x1617**: black keys (1024 bytes, monotonic).
   - Black or white is chosen by `(0x054A >> (k % 12)) & 1` (0x209C). The bits are C#, D#, F#, G#, A#, which confirms that key 0 is a C.

   Your "0x0EAA (648 B)" and "0x12CF (841 B)" are the ≤127 tails of these two tables.
2. **Speed → MIDI velocity (0..127)** (0x38CA). This uses the velocity-curve setting (param 2):
   - 0..3 selects one of four 256-byte curves at **0x3C40, 0x3D40, 0x3E40, 0x3F40**. The table is reached through a RAM pointer table at 0x20000480, copied from 0x45C4. The black-key pointer set is identical.
   - Curve 3 is essentially fixed at 100.
   - A param value ≥ 4 means a fixed velocity equal to the param value.
   - The factory default is curve 1 (0x3D40).
3. **0x4218 (128 B; your "0x4228" is from index 16)** is **not** a velocity table. It is the pitch-bend/mod **ramp-rate** table (0x3178: rate setting → step period, where setting 0 → 2000), stepped every 2 timer ticks. Return targets are at 0x453E: PB → 0x40, mod → 0.

Note number = k + 12·octave + transpose − 160, wrapped into 0..127 (0x38F4). The octave setting is centred at 0x10, with range 0x0C..0x14 (±4).

---

## 6. Non-obvious things a replacement firmware must do

- **Drive lines are push-pull, one-hot HIGH.** Keep all 8 low between scans. Never drive two at once, because two keys pressed on different lines could short two outputs through the matrix if there is no diode per contact. Add a short settle delay (the stock FW has none and gets away with it at 48 MHz, but slower pins or long ribbons may need ~1 µs).
- **Sense lines and buttons have no internal pulls.** Keep PMD=00 (plain input). Don't use quasi-bidirectional mode (11) on the PB sense lines: its weak pull-up would fight the external pull-downs and read every key as pressed.
- **LEDs are most likely active-low (sinking) on PA10–PA15.** Drive PA10–PA15 **high** at boot for "off". Evidence:
  - The factory-test LED chase pulls exactly one of PA10..15 low at a time (0x2E6C).
  - The octave progression only makes sense if low means lit.
- **Bicolour octave LEDs.** Each octave LED is a two-colour LED: one colour on a PWM pin (PA14 / PA15) and the other on a static GPIO (PA10 / PA11). Stock states (`led_set`, 0x3322) for Oct−, mirrored for Oct+:
  - centre: GPIO off, PWM-colour at a 11% duty glow
  - ±1: GPIO-colour only
  - ±2: both colours
  - ±3: PWM-colour only
  - ±4: PWM-colour blinking at ~1 Hz (CNR=0xFFFF, CMR=0x7FFF; 12 MHz/92/2/65536)

  These are probably green/amber/red, but the colours are unverified.
- **Sustain/Edit LEDs.** "Off" is CMR=0xE3, which is an 11% duty **dim glow** if active-low. "On" is CMR=0, which is about 100% lit.
- **PA0** needs OFFD bit 16 set and GPA_MFP[0]=1 before using ADC0.
- **The factory-test loopback at boot (0x2BF8).** The stock FW walks a one-hot pattern across the drive bus and checks whether PA1–7 and the PB sense byte echo it, which only happens on a test fixture. On a match it enters an endless LED-chase/ADC test loop. That loop is harmless and can be dropped, but it shows that the drive bus is briefly toggled at power-up.
- **Protected registers.** The stock FW unlocks the protected registers and **never re-locks** them.
- **Crystal stability.** It doesn't check the XT1/PLL stable flags. A replacement should poll CLKSTATUS.
- **LDROM boot (CBS=0).** CONFIG0 boots from LDROM, so the replacement APROM is entered via the LDROM. The vector table is at 0x0 of APROM, which is normal; see `ldrom.md` for how the jump happens.

---

## 7. Open questions (need probing on the board)

1. **Crystal frequency.** 12 MHz is effectively certain from PLL/USB maths, but it is worth reading the can marking or scoping XT1_OUT.
2. **LED polarity and colours.** Confirm active-low on PA10–15 and identify the two colours of each octave LED. Check whether there is a separate power LED that the MCU doesn't control (none is driven by firmware).
3. **Key order and contact order.**
   - Confirm key 0 = lowest C, and the k = line + 8·pair mapping (e.g. press the lowest key and see which PA8/PB pins toggle).
   - Confirm which physical contact (top or bottom) is the odd bit for keys 0–15 and the even bit for keys 16–31. The software swap suggests the PCB routes the upper half differently.
4. **External pulls.** Confirm the AR1–AR6 arrays are pull-downs on PB0–5, PB8 and PB12 and pull-ups on PA1–7, and whether there are diodes per contact (this decides whether multi-line drive is safe).
5. **PB.15 (pin 43).** Is it routed to a pad or test point? It would make a convenient debug-UART TX or spare input.
6. **ADC_S = 10b** with ADC_N=0 is out of spec by the TRM. The value is reserved in V2.02. Confirm what the silicon does, or just use the crystal.
7. **Volume knob wiring.** Check the pot's span (rail to rail?) to calibrate end-stops. The stock FW clamps <2 → 0.
8. **Power-up combo Sustain + Edit + Oct−.** It makes `main` exit. Does that serve an ISP/LDROM purpose? This is relevant to the bootloader work.
