# FCB1010 brain swap: ESP32-S3, powered over USB-C

2026-10-30. Design notes for replacing the Behringer FCB1010 main board with an ESP32-S3 board. The mains transformer, IEC inlet and power switch come out entirely: the unit runs from USB-C. The original switch/LED boards, expression pedals, MIDI board, jack board and enclosure stay.

How this fits into gemifi: [framework-research.md](../../core/framework-research.md). The FCB1010 becomes a fixed-device board file, flashed and tuned from the web editor.

Status: research only. Everything marked **(measure)** must be confirmed on a real unit before PCB layout.

## Sources

No Behringer schematic or service manual is public. The wiring below comes from:

| Tag | Source | What it gives us |
|---|---|---|
| JS | [jspyve/FCB1010-lib](https://github.com/jspyve/FCB1010-lib) | 8051 firmware for the *stock* board: how the latches, ADC, relays and LED multiplexing are really driven |
| FI | [mackatack/FCBInfinity](https://github.com/mackatack/FCBInfinity) | A Teensy++ main-board replacement with a published schematic (`Schematics/FCBInfinity Schematic.pdf`), installation notes and firmware. **The closest precedent; it was built and worked.** No licence, so reimplement it rather than copying |
| PU | practicalusage.com: [teardown](https://practicalusage.com/the-behringer-fcb1010-pedal-board/), [PU-2 Arduino brain](http://practicalusage.com/new-brain-for-pu-2-arduino-mega/), [alternate power](https://practicalusage.com/behringer-fcb1010-alternate-power-source/), [parts](https://practicalusage.com/behringer-fcb1010-ordering-parts/) | Board inventory, connectors, power and current draw |
| TEE | [TEE1010 (Teensy 4.1)](https://forum.pjrc.com/index.php?threads/tee1010-a-fcb1010-rebuild-with-a-teensy-4-1.68082/), [repo](https://github.com/ExpensiveNotes/TEE1010) | Problems running at 3.3 V, and how the pedals behave on 3.3 V |
| SCK / WINO | fcb1010.eu [single-cable kit](https://www.fcb1010.eu/downloads/fcb1010_singlecablekit.pdf), [Wino manual](https://www.fcb1010.eu/downloads/Wino_Usermanual.pdf) | The 4-pin MIDI lead, power, and rear-panel holes |

## What's in the box today

| Board | Connection | Notes |
|---|---|---|
| **Main board** (being replaced) | — | P80C32, EPROM, 24C16, ADC0832, 7805, TL7705, 74HC273 latches, transistor arrays. **It also carries the 3-digit display, about 13 status LEDs and both relays** (FI, PU) |
| Lower switch board: switches 1-5 + DOWN | 14-pin, 2.00 mm cable | Switches and LEDs. Apparently no series resistors (PU-2 added its own 330 Ω) |
| Upper switch board: switches 6-10 + UP | 14-pin, 2.00 mm cable | UP and DOWN have no LEDs |
| 2 pedal sensor boards | 3 wires each: GND, signal, +5 V (FI) | An LED + phototransistor reading a grey-gradient strip, about 5.7-5.8 mA each (TEE) |
| MIDI board | 4-pin lead, hot-glued | H11L1 opto + DIN jacks. Wino is powered from it, so it probably carries +5 V, GND, TX, RX (**measure**) |
| Jack board | 3 wires (common, SW1, SW2) to the relays on the main board | Amp channel switching, tip/sleeve |
| Mains section | 2 yellow wires, about 10 VAC | **Removed** |

The stock unit draws **85-92 mA from 9 V** (PU). With a linear 7805, that's about the same current at 5 V. So the whole original logic and LED load is about 90 mA, which leaves plenty of headroom for USB.

Since the display and status LEDs sit on the main board, **the new board has to put a display and status LEDs behind the same front-panel windows.** FCBInfinity's board was 160 × 40 mm, double-sided, with the digits and LED driver on the side facing the window (FI faq.md, install.md).

## Switch/LED cables

FCBInfinity's header map, read as a 2×7 header with pin 1 as the square pad. **Check the real wire order with a meter**: mackatack's first board had the upper connector wrong.

| Pin | LOWER (1-5, DOWN) | UPPER (6-10, UP) |
|---|---|---|
| 1 | LED common | LED common |
| 2 | LED common (second; FI tied it to pin 1) | LED6 |
| 3 | LED1 | LED7 |
| 4 | LED2 | LED8 |
| 5 | LED3 | LED9 |
| 6 | LED4 | LED10 |
| 7 | LED5 | SW6 |
| 8 | SW1 | SW7 |
| 9 | SW2 | SW8 |
| 10 | SW3 | SW9 |
| 11 | SW4 | SW10 |
| 12 | SW5 | UP |
| 13 | DOWN | switch common |
| 14 | switch common | switch common |

- **Switches:** each has its own wire, with a common return per board.
  - FI and TEE wire the common to GND and use pull-ups on the MCU.
  - The stock board scans them instead: 6 column lines on a latch, 2 sense lines (JS). We don't need to.
- **LEDs:** in the stock unit they're one 6×8 multiplex: 3 digit rows, 2 rows of switch/indicator LEDs, 1 status row (JS `leds.h`).
  - **Polarity is disputed.** FI drives the board commons from MAX7219 digit pins (which sink current), which means **common cathode**, and that worked. PU-2 describes common anode. **Measure with a diode tester.**
  - The lower board's two common pins are probably split LEDs 1-3 / 4-5 (an inference from the stock multiplex). Tying them together, as FI did, works.
- **Housings:** 2.00 mm, either JST PH-style or Molex. PU-2 found them hard to source (**measure** and identify).

## Proposed design

```
USB-C ──fuse──┬── 5 V rail ──┬── MAX7219 (all LEDs + display) ◄── 74AHCT125 ◄── ESP32 SPI
 (CC: 5.1k)   │              ├── pedal sensors ×2 ──► divider/RC ──► ADC1
              │              ├── MIDI board (H11L1, DIN) ◄──► UART1 (via level parts)
              │              └── relays/photoMOS ×2 ◄── GPIO
              └── 3.3 V LDO ──── ESP32-S3-WROOM-1 (native USB to the same USB-C)
switch boards (12 switches) ──► 1k series ──► GPIO (pull-ups to 3.3 V)
```

### MCU

- **ESP32-S3-WROOM-1 N8 or N8R2** (quad PSRAM). Avoid R8 (octal PSRAM), which uses GPIO 33-37.
- Native USB on GPIO 19/20 to the USB-C socket gives:
  - USB-MIDI;
  - first install over Web Serial (ROM download mode);
  - SysEx updates after that.
- BLE MIDI is available. Wi-Fi is optional (see power).

### LEDs and display

- **One MAX7219** drives everything, as in FCBInfinity:
  - 3 digits;
  - about 13 status LEDs;
  - 10 switch LEDs, using one digit line per switch board common.
- **Digits:** our own **common-cathode** 3-digit display (or 3 singles) on the new board. The stock digits are common anode (PU commenter) and don't suit the MAX7219, so they go too.
- **Level shifting:** run the MAX7219 at 5 V. Its VIH of 3.5 V is marginal for 3.3 V logic, so pass DIN/CLK/LOAD through a **74AHCT125** (5 V supply, TTL inputs).
- **Current:** set RSET for about 10-15 mA per segment. The datasheet's chart says roughly 30-50 kΩ (check the chart). Brightness can also be set in firmware.
- **If the switch boards turn out to be common anode,** the MAX7219 can't drive them directly. Fallback: a TLC5916/STP08 constant-current sink plus high-side PNPs for the commons, or one 74HC595 + ULN2803 per board. Decide after measuring.

### Switches

- 12 GPIOs, each with **external 10 kΩ pull-ups to 3.3 V** (long cables), a **1 kΩ series resistor** and an optional 100 nF capacitor. Board commons go to GND.
- Debounce in firmware. Hold time, long-press and double-tap are gemifi profile settings.
- **Put DOWN on GPIO0** (a strapping pin). Holding DOWN while plugging in USB then drops the ESP32 into ROM download mode, a recovery path no firmware bug can break. The web editor can say "hold DOWN and plug in".

### Expression pedals

- Each sensor has an LED-current setting and an output range; check both before choosing the parts (**measure**).
- **Default:** power the sensors from 5 V (as stock). The output goes through a divider (footprint for e.g. 10k/18k, scaling 5 V down to about 3.2 V), then 1 kΩ + 100 nF, into **ADC1** (GPIO1, GPIO2).
- **Option** (solder jumper): power them from 3.3 V with no divider. TEE1010 got a linear response that way.
- **Calibration:** in software, min/max learning plus a curve. This is where the 12-bit ADC and the gemifi feel pipeline beat the stock 8-bit ADC0832.

### Relays (amp switching)

- The stock relay coil voltage is **unknown** (**measure**). A hint suggests they may sit on the unregulated rail, which no longer exists.
- **Default: replace them with photoMOS solid-state switches** (e.g. Panasonic AQY212EH or Toshiba TLP222A class).
  - About 3-5 mA each.
  - Silent.
  - Isolated, which also avoids ground loops between amp and USB host.
- **Footprint option:** a 5 V signal relay (e.g. Omron G5V-1 class, about 30 mA coil) with a flyback diode and NPN driver, for amps that dislike the photoMOS on-resistance (a few Ω to tens of Ω).

### MIDI

- Keep the MIDI board and power it from our 5 V rail.
- **MIDI OUT:** use 3.3 V drive with the MIDI spec's 33 Ω / 10 Ω resistors. Check whether the MIDI board already has series resistors (**measure**); put bypassable footprints on our side.
- **MIDI IN:** the H11L1's output is open collector. The pull-up was probably on the main board, so put it on ours, to 3.3 V. If the MIDI board pulls up to 5 V itself, add a 5 V-tolerant buffer (74LVC1G17) footprint.
- **Use UART1** on free pins, **not UART0 (GPIO43/44).** The S3 ROM prints its boot log on U0TXD, which would send junk out of MIDI OUT at every reset. Bring GPIO43/44 out to a debug header instead.

### USB-C and power (mains removed)

- **USB-C receptacle (USB 2.0):**
  - 5.1 kΩ Rd on CC1 and CC2 (a sink);
  - USBLC6-2 ESD protection on D+/D−;
  - a 750 mA polyfuse on VBUS;
  - about 10 µF bulk capacitance at the connector (USB inrush limit) plus local decoupling.
- **3.3 V:** an LDO rated 500-600 mA or more (AP2112K-3.3 class). VBUS minimum is about 4.75 V, so dropout is fine.
- **Reading the CC voltage:** route CC1/CC2 into ADC1 as well (through high-value dividers). That tells us whether the port offers 500 mA, 1.5 A or 3 A, so the firmware can **turn Wi-Fi on only when the port can supply it**.
- **Mounting:** a **panel-mount USB-C extension** through a blanking plate over the old IEC inlet hole, cabled to the board. The board doesn't have to reach the rear panel.
- **Old power-switch hole:** a spare rear-panel button (BOOT/recovery, or user-assignable) or a blanking plug.

### Power budget at 5 V

| Load | Typical | Worst case |
|---|---|---|
| ESP32-S3, USB + BLE, no Wi-Fi | 50-100 mA | 150 mA |
| MAX7219 + LEDs (multiplexed; only one digit row lit at a time, up to 8 segments × ISEG) | 40-80 mA | ~120 mA at 15 mA/segment |
| Pedal sensors ×2 | 12 mA | 15 mA |
| PhotoMOS ×2 | — | 10 mA |
| (5 V relays ×2 instead) | — | 60 mA |
| MIDI board | ~5 mA | 10 mA |
| **Total, no Wi-Fi** | **~110-200 mA** | **~305-355 mA** |
| Wi-Fi TX peaks | | +250-350 mA |

- **Without Wi-Fi:** this fits a 500 mA USB 2.0 port with margin. For comparison, the stock unit used about 90 mA.
- **With Wi-Fi:** worst case goes over 500 mA. So Wi-Fi only turns on when the CC pins advertise 1.5 A or more, or the user forces it on.

### GPIO map (first pass)

| Function | GPIO |
|---|---|
| DOWN (also BOOT/recovery) | 0 |
| SW1-10, UP | 4, 5, 6, 7, 8, 15, 16, 17, 18, 21, 47 |
| Pedal A, Pedal B (ADC1) | 1, 2 |
| CC1, CC2 sense (ADC1) | 9, 10 |
| MAX7219 DIN, CLK, LOAD (through AHCT125) | 11, 14, 38 |
| MIDI TX, RX (UART1) | 12, 13 |
| Relay/photoMOS 1, 2 | 39, 40 |
| USB D−, D+ | 19, 20 |
| Debug UART0 header | 43, 44 |
| Spare | 41, 42, 48 |
| Avoid (strapping) | 3, 45, 46 |

This is a first pass. Recheck it against the WROOM-1 datasheet and the final routing.

## Firmware fit (gemifi)

**Board file `fcb1010-s3`.** This is a fixed device, so hardware setup is skipped. It describes:
- 12 switches, 10 switch LEDs, 13 status LEDs, a 3-digit display;
- 2 pedals;
- 2 contact outputs;
- DIN MIDI, USB-MIDI and BLE.

**Profile** (runtime, edited from the web editor):
- presets and banks, with no 2 KB limit;
- per-switch actions (PC/CC/note/toggle/momentary/long-press);
- pedal curves and calibration;
- relay behaviour;
- display text.

**Importing UnO setups:** the editor already parses UnO dumps, so a stock or UnO user can bring their setup across.

**Updates:**
- first install over Web Serial, or the DOWN-held recovery mode;
- after that, gemifi SysEx updates over USB-MIDI;
- DIN SysEx also works, but a full ESP32 image would take minutes.

## Measure before layout

1. Wire order and housing type of both 14-pin cables.
2. LED polarity on the switch boards, the lower board's two commons, and whether there are series resistors.
3. Relay coil voltage and resistance, part number, and the rail it ran from.
4. Pedal sensor pinout, LED current, and output swing at 5 V and at 3.3 V supply.
5. MIDI 4-pin pinout, where the MIDI-IN pull-up is and its voltage, and where the MIDI-OUT resistors are.
6. **Main-board outline and mounting holes:** three rear screws + the 7805 tab screw (PU). Also the display window, the status-LED positions and the height clearance.
   - Tip: scan the old board on a flatbed at 600 dpi next to a ruler, and trace it in KiCad.
7. Rear-panel cutouts: IEC inlet, power switch and the spare hole next to the MIDI jacks (SCK).
8. Jack-board ground path: MIDI board vs chassis. Keep the amp jacks isolated from USB ground if you use photoMOS.

## Next steps

1. Open the unit and work through the measurement list. Photograph and scan the main board.
2. **Breadboard prototype:**
   - an ESP32-S3 DevKitC wired to one switch board, one pedal and the MIDI board through the original connectors;
   - a MAX7219 module plus a 74AHCT125.
   - This proves LED polarity, the pedal range and MIDI I/O at 3.3 V.
3. Draw the schematic in KiCad from the blocks above, then lay out the board to the scanned outline.
4. Firmware: the gemifi `fcb1010-s3` board file plus the UnO dump importer in the editor.
