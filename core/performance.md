# Component performance

What each component in a device YAML costs: CPU, latency from the physical action to MIDI out, MIDI traffic, RAM, and the YAML knobs that trade them. For how the components work, see [README.md](README.md). Numbers are tagged:
- **measured**: from a device's boot report (Code 61 build 48, 2026-11-03) or a build's symbol sizes;
- **derived**: worked out from the code and the chip's clocks, not timed on hardware.

## Where the time goes

| Context | Runs | What runs there |
|---|---|---|
| Scan tick (timer interrupt) | every `scan_interval` | key matrix and switch matrices (software scan), HC165/HC595 refresh and flush, encoders on MCU pins, shift registers and switch matrices |
| Frames (DMA interrupt, stm32f1 `hardware_scan.matrix`) | every 8 frames (672 us at the Code 61's 84 us frames) | key and switch-matrix changes, encoders on switch matrices |
| ADC sweep (DMA interrupt, `hardware_scan.analog`) | every sweep (200 us on the Code 61) | copies the readings, keeps the pads' peaks |
| Main loop (`gm_poll`), one pass | as fast as it goes (measured worst pass on the Code 61: 491 us) | keys' MIDI out first, then buttons (1 ms), the protocol, DIN, presets, I2C hubs (1 ms), analog (1 or 2 ms), pads (1 ms), encoders, LEDs (10 ms), TM1628 and RGB writes |

A key's note goes out on the first main-loop pass after the scan sees it; USB then takes up to one 1 ms frame. Anything slow in the main loop (an I2C read, a TM1628 write, a software mux settle, a flash write) delays the next pass, so the keys wait behind it too. The stm32f1 boot report (log level info, at 2.5 s and 10 s) gives the worst time of each part: `scan keys/panel/encoders`, `poll proto/keys/analog/pads/encoders/led update/led writes`, and the hardware scan's interrupt maxima. The NUC1xx logs `scan load` as a percentage of the tick.

## Platforms

| | Code 61 (STM32F103RB) | Keystation Mini 32 (NUC120LC1BN) | ESP32 / S3 | ESP8266 |
|---|---|---|---|---|
| CPU | Cortex-M3, 72 MHz | Cortex-M0 at 48 MHz (rated 50 MHz), no divide instruction | Xtensa 240 MHz | 80/160 MHz |
| RAM used / total (measured) | 12.8 KB / 20 KB | 1.8 KB / 4 KB (512 B kept for the stack) | n/a | n/a |
| Flash used (measured) | 61 KB of the 100 KB app slot | 25.7 KB of 32 KB | n/a | n/a |
| One ADC conversion (derived) | 4.5 us (ADC2, 41.5-cycle sample) | ~5 us (6 MHz ADC clock) | tens of us (`analogRead`) | ~100 us (`analogRead`, blocks; #71) |
| Fast pin paths (one store or load per row, run of columns, decoder output, TM1628 pin) | yes | yes | no: one HAL call per pin (#71) | no |
| Hardware scan | `hardware_scan:` (timer + DMA) | no | no | no |

## Per component

"Pass" is one main-loop pass. RAM is per item, besides its params: every param value is 4 bytes in `gm_cfg` and 8 in the store's record buffer (`rec[]`), so an array param costs 12 bytes per item.

| Component | CPU | Action → MIDI | MIDI it sends | RAM | Knobs |
|---|---|---|---|---|---|
| Key matrix, software scan | each tick: rows × (`settle` + row overhead). Keystation 8 rows: ~21 us of each 125 us, 17% (derived). STM32 fallback, 16 key rows + 5 panel rows at 3 us: 48-63 us of 400 (#69) | `scan_interval` to see the second contact, + the rest of a pass, + ≤ 1 ms USB | Note On / Off, 3 B each (2 on DIN with running status) | 5 B per key, 8-16 B per row, 128 B key queue | `scan_interval`: velocity step and latency vs CPU; `settle`: ghost notes vs CPU; `velocity: { fast, slow }` |
| Key matrix, `hardware_scan.matrix` | no tick work; ~12 masked compares per frame where nothing moved; measured interrupt max 45 us per 8 frames (at 36 us frames) | one frame (row_time × rows: 84 us on the Code 61, 3.5 us × 24; 36 us at the 1.5 us minimum) + pass + USB | as above | + 220 B (scan plan, last frame) | `row_time`, `settle`, `precharge` (README "Hardware scan") |
| Button on an MCU pin | read every 1 ms, a few cycles | ≤ 1 ms + pass + USB; a release counts after 16 ms open | one message per press / release (CC, note, program, ...) | 6 B, + 2 params; 72 B shared | none |
| Button on a switch matrix | the matrix's rows each scan (below); the button read is the cache | as above, + up to one full matrix scan (`rows` / `rows_per_scan` × 2 ticks; one frame with hardware scan). A press counts once every scan in a 1 ms sample read it closed; any scan that reads it closed holds off the release | as above | as above | `rows_per_scan`, `settle` of the `switch_matrix` |
| Button on an I2C expander | the chip's read (below) | as above, + 1 ms (the chip is read after the buttons in a pass) | as above | as above | none |
| `cc_ramp` action, bend buttons | every ms: a loop over all buttons, plus one divide per ramping button (~5 us/ms on the Code 61's 55 buttons, derived) | – | a CC per step while ramping; pitch bend at most every 2 ms (`bend.glide`) | in the 6 B | `ramp.time`, `bend.glide` |
| Encoder on MCU pins, HC165 or a switch matrix | one quadrature step per tick (or frame); main loop: per pass, only when it moved | ≤ one pass after the detent completes | one CC per pass that saw movement (relative modes carry several detents in one); EVENT too with `debug.events` | 23 B per scanned encoder (+8 with acceleration), 10 B per encoder or twin | `scan_interval`: each quadrature state must last one tick, so ~4 ticks per detent at best: about 500 detents/s at 500 us, fewer with uneven contacts (derived); `steps_per_detent`; `acceleration:` |
| Encoder on an I2C expander | stepped from the main loop on the 1 ms cache | ≤ 1 ms + pass | as above | as above | at most one quadrature state per ms: ~250 detents/s at best, much less in practice. Use MCU pins for encoders |
| Analog input (pot, fader, wheel, aftertouch) | software: one conversion (+ the mux `delay` when its channel changes) per 2 ms for the CC inputs in turn, plus every `pitch_bend` / `channel_pressure` input each time. `hardware_scan.analog`: none, all inputs every 1 ms from the sweep | filter: each reading moves the value 1/4 of the way (EMA). A slow move lags ~3 readings; a full-range slam needs ~22 readings to send its last value (derived): 22 ms with hardware scan, 44 ms with one input, 2 ms × N × 22 with N CC inputs in software (8 faders: ~350 ms) | at most one message per reading when it moves past the hysteresis (24 of 32 counts per CC step); pitch bend 14-bit, steps of 6 raw counts, every 2 ms (1 ms hardware) | 14 B, + 2 params; +2 with `takeover:`, +5 with `touch:` | `adc_min` / `adc_max` (calibration, also scales the bend steps); the mux `delay`; `hardware_scan.analog`; fewer CC inputs per ADC for faster updates |
| Pad (FSR) | every 1 ms: one conversion per pad, mux settles once per channel (16 pads on the Code 61 in software: ~0.15 ms of every ms). With `hardware_scan.analog`: a peak lookup per pad | threshold crossing (≤ 1 ms) + `pad.window` (default 3 ms) | Note On / Off; poly pressure at most every 10 ms per pad (`pad.aftertouch`), so 16 pads held = 4.8 KB/s, more than DIN carries (poly pressure isn't coalesced) | ~16 B (+2 with twins), + 1 param | `pad.window` (latency vs velocity accuracy), `pad.threshold`, `pad.retrigger`, `pad.aftertouch` |
| XY pad | one conversion per ms, in software (ADC2 on stm32f1) | each axis new every 2 ms; a touch counts after ~4 ms | as an analog input | a hub, + 2 analog inputs | `touch_min` / `touch_max` |
| LED (any pin) | levels worked out every 10 ms; an LED is rewritten only when its level (or a blink phase) changes. Measured on the Code 61 (67 LEDs): 100 us per 10 ms | a button's LED: ≤ 10 ms + the hub's write; host feedback: 13 LEDs per pass | – | 3 B (+8 with host feedback) | `idle:` / `active:` (no cost), `feedback:` |
| LED on an MCU pin | one HAL write on change | – | – | – | – |
| LED on HC595 | the chain is shifted out in the scan tick when any bit changed: 3 HAL calls per bit (~0.4 us on M3, ~0.8 us on M0, derived; #70) | ≤ one tick after the LED update | – | hub only | – |
| LED on MCP23017 / PCF8574 | one write on change, every 1 ms at most: 3 bytes (~80 us at 400 kHz) / 1 byte (~50 us) | ≤ 1 ms after the LED update | – | hub only | – |
| LED on TM1628 | changed bytes only, at most 9 bus bytes per pass, never on the LED-update pass: ~110 us per pass at most, one LED ~40 us (derived, stm32f1); measured `led writes` 122 us | ≤ 10 ms + one or two passes | – | 2 B per RAM byte | `ram_bytes` (start-up write only), `intensity` |
| LED on `rgb_serial` (Code pad LEDs) | one 6-byte message per 5 ms at most, from the main loop; the UART runs by DMA or interrupt | one LED per 5 ms: all 16 pads changing take 80 ms | – | 2 B per 8 colour bits | `count`, `dma` |
| Displays (14- and 7-segment) | formatted with the LED update (every 10 ms); only changed segments are written, through the hubs' bus budget (a 6-character change on a TM1628 is ~14 bus bytes: 2 passes; a MAX7219 digit is one frame, ~8 us a chip; a TM1637 digit ~180 us, one a pass) | ≤ 10 ms + 1-2 passes | – | small | `daw:` |
| DIN MIDI | ~1 us per byte by interrupt (or DMA) | queue-dependent: DIN carries ~3 bytes per ms (320 us a byte) | the device's own messages copied (`midi.din_out`), thru, USB | 512 B TX + 256 B RX queues | `mode`, `dma`, `thru`, `controller_out`, `usb_to_din` (README "DIN MIDI") |
| Presets | a switch reads the slot's record from the store and applies it (a flash read, no write); a flash write (a host's COMMIT or PRESET_*, or `write_to_flash`) can stall the CPU, and the key scan, for 20-40 ms | a switch: within the pass; the display shows the name | – | 8 B per param value (record buffer) | `write_to_flash:` (README "Writes the device makes on its own") |

## Hubs

| Hub | Cost | Knobs |
|---|---|---|
| `decoder` | one store per row with a set/reset register (stm32f1), else a HAL write per address line on change | – |
| `switch_matrix` (software) | each scan, every other tick: `rows_per_scan` rows (default all) × (`settle` + a load); a column run on one input register is one load per row | `settle` (default 1us), `rows_per_scan` (fewer rows per scan: less tick time, slower buttons) |
| `cd74hc4051` / `cd74hc4067` | `delay` (default 2us) busy-wait each time the channel changes, then the read or conversion | `delay`: give muxes that share select lines the same one |
| `sn74hc165` | refreshed every scan tick: SH/LD pulse with a 1 us wait, then 3 HAL calls per bit: ~4-8 us per chip per tick (derived; #70) | `sr_count`, `scan_interval` |
| `sn74hc595` | written in the scan tick only when an output changed | `sr_count` |
| `mcp23017` | read every 1 ms if it has inputs: two blocking transactions, ~120-150 us at 400 kHz (~12-15% of the main loop, derived; #72). A chip with only outputs isn't read. Outputs: one write on change | put inputs on as few chips as possible |
| `pcf8574` | read every 1 ms if it has inputs: one 1-byte transaction (~50 us). Outputs: one write on change | as above |
| `tm1628` | see LED on TM1628 | – |
| `rgb_serial` | see LED on `rgb_serial`; plus a keepalive byte every 50 ms and one LED sent again every 50 ms when idle | – |
| `xy_pad` | see XY pad | – |

## MIDI traffic

- **USB.** Messages are packed into 64-byte packets (16 events) and handed to the endpoint at the next `hal_midi_poll` (three per pass); stm32f1 queues 4 packets, NUC1xx 2. A packet the host doesn't take within 50 ms means no host: the queue is dropped.
- **DIN.** ~3,125 bytes/s. Running status makes a note or a CC 2 bytes. Pitch bend, channel pressure and CCs are coalesced to their latest value while the queue is long, so wheels and faders can't build up a backlog (README "DIN MIDI"); notes and poly pressure aren't.
- **EVENTs** (`debug.events` on, for DAW scripts) are SysEx, ~20 bytes each (7 USB events), one for every key, button, encoder movement and analog value sent: turn them on only when a host script reads them.
- **What sends a lot:** a pitch wheel moved steadily (up to 500 messages/s, 1,000 with `hardware_scan.analog`), several pads held with aftertouch, encoders with acceleration in absolute mode (one CC per pass while turning).

## Knobs at a glance

| YAML | Lower / smaller | Higher / larger |
|---|---|---|
| `scan_interval` | finer velocity, faster encoders, more scan CPU | less CPU, coarser velocity (one step per tick: fortissimo, 2-4 ms between contacts, is 16-32 steps at 125 us) |
| `key_matrix.settle`, `switch_matrix.settle` | less busy-wait per row | fewer ghost notes on slow columns |
| `hardware_scan.matrix.row_time` / `settle` / `precharge` | shorter frames (36 us at 1.5 us rows), finer timing | lightly pressed carbon contacts can read open; the Code 61 uses 3.5 / 3 us (84 us frames) |
| `hardware_scan.analog` | – | on: no conversions in the main loop, all inputs every 1 ms, pad peaks between passes |
| mux `delay` | faster software analog reads | clean readings on high-impedance sources |
| `switch_matrix.rows_per_scan` | less scan time per tick | each button seen sooner |
| `pad_settings.window` | Note On sooner | the peak (velocity) found more reliably |
| `encoder acceleration:` | – | more steps per detent when fast; no extra MIDI messages (one per pass either way) |
| `din_midi.dma` | – | on: no UART interrupt per byte, costs DMA channels |
| `write_to_flash` | off (default): the device never stalls on its own | on: a stall of 20-40 ms, but only when idle |

## Web config tool (web/updater)

Measured and derived on 2026-11-05 from the client code and the generated configs (Code 61: 38 params, 251 values, schema 5,250 B; Keystation: 19 params, 31 values, 1,426 B). The protocol is one request at a time, so round trips (about 3 ms each, derived) set the time, not bandwidth.

| Flow | Code 61 | Keystation |
|---|---|---|
| Connect: HELLO, component list, schema (64 B windows), all values, preset list | ~302 round trips, ~0.9 s | ~66, ~0.2 s |
| Change a value | 1 SET_PARAM (number fields: one per keystroke), + the preset list again when the dirty flag flips | same |
| Save | 1 COMMIT (flash is written only then) | same |
| Firmware upload | one 256-byte FW_DATA per round trip: ~2.5 s per 64 KB | same |

## Open work

Already filed:
- #68: an index from CC or note to component, for host feedback.
- #69: overlap row processing with the next row's settle in the software scan.
- #70: faster HC165/HC595 hubs.
- #71: ESP fast GPIO paths, ESP8266 `analogRead`.
- #72: MCP23017 polling. Output-only chips are no longer read; interrupt-driven reads remain.
- #73: smaller leftovers (`gm_cfg` as `int16_t`, ADC buffers, CRC, etc.).

Proposals:
- **Analog rate.** In software, the CC inputs share one conversion per 2 ms, so 8 faders on one mux update every 16 ms and settle on a slam in ~350 ms. Reading every input each 2 ms where a conversion is cheap (STM32, NUC: ~5 us plus the mux `delay`) would bring that to ~44 ms for ~0.1 ms of CPU per 2 ms. A faster filter step for large moves would help as well. Both change the MIDI stream, so they need trying on hardware.
- **Ramps.** `ramps_ms` walks every button each ms to find `cc_ramp` ones; a mask of ramping buttons would skip it (~0.5% CPU on the Code 61).
- **Web tool.** These are client-only:
  - List components only when the host-feedback view opens. This would take ~170 round trips off connect on the Code 61, so settings show after ~0.4 s.
  - Keep the device info while HELLO is refreshed. Today the Refresh button reloads everything and clears the unsaved badge.
  - Coalesce value re-reads. A preset delete or move can read every value twice.
  - On a preset switch, re-read only the preset params, not the globals (~25 round trips instead of 44 on the Code 61).
  - Re-read only the active preset's info when only its dirty flag changed.
  - Debounce number fields.
  - Cache the schema per board and firmware version.
  - After an A/B upload, wait for BOOTED instead of polling HELLO.
- **Protocol.** These belong to the protocol work:
  - A request window (pipelining).
  - Larger schema windows.
  - A multi-id value read.
  - A ranged GET_COMPONENT.
  - A schema hash in HELLO.
