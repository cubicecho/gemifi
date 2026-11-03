# Code 61: the fastest way to scan it

Written 2026-11-03 (branch `perf/code61-dma-scan`, fw 0.2.40-0.2.52, tried on hardware as builds 39-48). It covers what limits the scan on this board, what the chip offers, and what was built: the matrix and the analog inputs scanned by timers and DMA (core/README.md "Hardware scan"), and cheaper LED writes. Results on hardware are in §7.

## 1. Where the time went (fw 0.2.27, measured on hardware)

| Part | Where | Cost | Effect |
|---|---|---|---|
| 16 key rows | TIM2 interrupt, every 400 us | ~123 us a scan: 16 × (3 us settle + ~4.7 us code) | 400 us velocity steps; 30% of the CPU |
| Panel + encoder rows | the same, every other tick | ~58 us | encoders sampled every 800 us |
| 16 pads | main loop, every 1 ms | ~369 us a pass: 16 conversions + 8 mux settles, all waited out | each pad read once a millisecond; a peak between reads is lost |
| 9 faders | main loop, one every 2 ms | ~10 us each | each fader read every 18 ms |

The rows' 3 us settle is the weak pull-up: once a closed key has pulled a column low, only the MCU's internal pull-up brings it back, at 30–50 kΩ (datasheet table 35). Against the column's capacitance that's microseconds. The ~4.7 us of code per row is the generic path running from flash with 2 wait states.

## 2. The chip (STM32F103RBT6) and what it offers

From the datasheet (DS5319, saved in `~/windows/_re/DS_stm32f103rb.pdf`), RM0008 and ES0340:

- **Clock:** 72 MHz from 8 MHz × 9, the only setting that also gives USB its 48 MHz (÷1.5). No overclocking.
- **Running code from RAM** is no clear win on the F1. SRAM code goes over the S-bus and shares it with data, and forum benchmarks show it from about the same to 40% slower than flash with prefetch ([ST community](https://community.st.com/stm32-mcus-products-25/stm32-performance-while-running-from-ram-115201)). Not pursued.
- **DMA1, 7 channels.** Timers can pace transfers to and from GPIO: a timer event writes BSRR (set/reset, so other pins on the port are untouched) or reads IDR ([ST community: DMA GPIO](https://community.st.com/stm32-mcus-products-25/dma-gpio-stm32f103-52865)). Request map (RM0008 table 78, confirmed against [libmaple's notes](https://juju.nz/src/juju/libmaple/src/branch/master/notes/dma-stm32f1.txt)): ch1 ADC1 / TIM2_CH3 / TIM4_CH1; ch2 TIM1_CH1 / TIM2_UP / TIM3_CH3; ch3 TIM1_CH2 / TIM3_CH4 / TIM3_UP; ch4 TIM1_CH4 / TIM4_CH2 / USART1_TX; ch5 TIM1_UP / TIM2_CH1 / TIM4_CH3; ch6 TIM1_CH3 / TIM3_CH1; ch7 USART2_TX / TIM2_CH2 / TIM4_UP. TIM1 alone has five requests on five channels (UP, CH1–CH4), which is what makes a multi-step row sequence possible.
- **ADC:** 12-bit, at most 14 MHz and 1 Msample/s. At 72 MHz the clock is PCLK2 / 6 = 12 MHz. Scan mode converts a sequence of up to 16 channels per trigger, with DMA. Regular-group triggers include TIM4_CC4, TIM1_CC1–3, TIM2_CC2 and TIM3_TRGO. The RB also has ADC2, for dual simultaneous conversions (not used yet). Maximum source impedance for 1/4 LSB (table 48): 11.4 kΩ at 13.5 cycles, 25.2 kΩ at 28.5, 37.2 kΩ at 41.5 (what the stock firmware uses), 50 kΩ at 55.5.
- **Errata (ES0340) that matter:** don't clear a DMA channel's global flag while it runs (2.4.1); DMA registers are 32-bit only (2.4.2); a compare event can be missed when CCR changes between consecutive counts (2.7.3, not an issue with fixed CCRs); trigger outputs need the receiving peripheral clocked first (2.7.2); injected conversions can glitch PA0 (2.6.1, so use the regular group, PA0 being a pad mux).

## 3. The other chips

There's no board photo of the Code 61's main board, and an online search found no teardown. These come from how the stock firmware drives them (hw-map.md):

- **Row decoders:** 3-to-8 decoders (74HC138-class) on PB3–PB5 (address) and PB6/PB7 (group): 24 outputs, 23 used (keys 0–15, panel 16–20, encoders 21–22). Output 23 is probably unconnected, but that's unconfirmed.
- **Matrix:** columns PB8–PB15 with the MCU's pull-ups, active low. The stock firmware's precharge (below) suggests diodes in the matrix, but that's unconfirmed.
- **Analog muxes:** three 8:1 (4051-class) on the shared select lines PC0–PC2, commons PA7 (faders 1–8), PA0 / PA1 (pads). No enable line. Our 4 us settle works; the part number and on-resistance are unknown.
- **LED drivers:** two TM1628-class chips, bit-banged on PC10/PC11 with strobes PA15/PC12. Their own key-scan inputs aren't used. They aren't on the scan path.
- **Pad LED board:** its own controller, on USART2 TX (PA2) at 115200 baud (hw-map.md, core/README.md "RGB serial LEDs").

A photo of the main board would confirm the decoder, mux and diode parts.

## 4. Options, and what was done

| Option | Gain | Status |
|---|---|---|
| **Matrix by TIM1 + DMA** (`hardware_scan.matrix`): TIM1 counts rows; its update stores the next row's select word into GPIOB_BSRR (ch5) and CC2 copies GPIOB_IDR to a RAM ring (ch3). The CPU only compares finished frames, 4 at a time | no CPU per row; keys timed per frame: 84 us (24 rows × 3.5 us) instead of 400 us; encoders sampled every 84 us instead of 800 us | **built** (`platform/stm32f1/src/scan_dma.c`) |
| **Precharge**, the stock firmware's trick: between rows the columns are driven high for a moment (CC3 → ch6 writes GPIOB_CRH to outputs, CC1 → ch2 back to pulled inputs after the next select), so a read only waits for a closed key to pull a column down | settle 3 us → about 1 us: 36 us frames (11× the stock resolution) | **built**, the Code 61 default (no ghost or missing notes on hardware, §7) |
| **Analog by TIM4 + ADC1 + DMA** (`hardware_scan.analog`): TIM4 CC2 stores each mux step (ch4), CC4 triggers ADC1's scan of PA7, PA6, PA1, PA0 after the 4 us settle, DMA ch1 copies the readings. The core keeps the latest reading and, for the pads, the strongest since their last look | the main loop's ~370 us of pad reading is gone; every pad and fader every 200 us; faders sent every 1 ms instead of every 18 ms; strike peaks between passes kept | **built** (`platform/stm32f1/src/adc_dma.c`) |
| Dual ADC (ADC1 + ADC2 simultaneous) | sweep 200 → ~110 us | later, if the pads want it |
| Shorter ADC sample time (28.5 cycles) | sweep 200 → ~160 us | needs the pad and fader source impedance (≤ 25 kΩ with the mux) |
| Code in RAM, overclocking | none / impossible | rejected (§2) |
| A faster software scan | some | superseded: the DMA scan leaves the CPU nothing to speed up |

Velocity is now worked out in microseconds (it was 0.1 ms steps), so the finer key timing gives finer velocities at the fast end. Fortissimo is around 2–4 ms between contacts: that's 5–10 steps at 400 us, 24–48 at 84 us and 55–110 at 36 us. For comparison, a [synth-DIY measurement of a Fatar keybed](https://synth-diy.org/pipermail/synth-diy/2016-April/151840.html) found a 1 ms scan gives only ~22 distinct loud velocities and 125 us about 56, and [MIDIbox's fast scan](https://forum.midibox.org/t/tutorial-029-fast-scan-matrix-for-velocity-sensitive-keyboard/16246) runs at ~300 us.

## 5. Costs

- CPU: one interrupt per 8 matrix frames (every 288 us at 36 us frames). A frame where nothing moved (nearly all of them) is 12 masked word compares; a frame with changes handles only the key pairs that changed, plus the panel and encoders when their rows did. One interrupt per ADC sweep (200 us) copies 32 readings.
- DMA: channels 1–6 (7 stays with the pad-LED USART). With both scans on, a 5-pin MIDI out on USART1 would need an interrupt-driven sender (its TX DMA is channel 4).
- Flash +6 KB (40.4 KB), RAM +3 KB (8.3 KB of 20).

## 6. Hardware test

The boot log (Config tab, log level info) at 2.5 s and 10 s now has:
- `hw scan frame 36000 ns, rows 24` (84000 without precharge): the matrix is on DMA. `hw scan irq max N us, overruns M`: overruns should stay 0 apart from flash writes (saving settings).
- `hw adc sweep 200 us, irq max N us`, `hw adc overruns M, resyncs K`: resyncs should stay 0.
- A WARN `hw scan: ...` / `hw adc: ...` line means it fell back to the software scan, and says why.

What to try:
1. Every key plays the right note; soft to hard gives a smooth velocity range; no ghost or stuck notes, including chords and fast trills.
2. Panel buttons (quick taps too), the 8 encoders turned fast both ways, sustain.
3. Pads: soft and hard hits, fast rolls on one pad; faders end to end.
4. The precharge build: the same, and especially chords (several keys held on one row) and fast glissandos, for ghost notes.

## 7. On hardware (2026-11-03)

All of this was tried on the unit, from the steady-state (10 s) boot report:

| | Build 34 (before) | Build 48 (fw 0.2.52) |
|---|---|---|
| Key timing step | 400 us | 36 us (precharge) |
| Scan tick | 128 us of every 400 | 2 us |
| Matrix / ADC interrupts | – | at most 45 us / 22 us; 0 overruns, 0 resyncs |
| Main loop, worst pass | ~1.6 ms | 491 us, none over 1 ms |
| LED update + writes | ~495 us in one pass | 100 us + 122 us, on separate passes |

- Precharge: no ghost or missing notes with chords and fast runs (builds 44-48), so it is the Code 61 default. Without it (build 39, 84 us frames) everything worked too.
- Blinking LEDs (DAW scripts) still blink with the LED cache.

**LED writes** (`core/src/vpin.c` `tm_flush`, `controller.c` `leds_update` / `leds_apply`): the TM1628 RAM used to be rewritten whole on any change (17 + 9 bytes at 2 us a bit, ~420 us). Now only changed bytes go out at fixed addresses (one LED = 3 bus bytes, ~50 us), at most 9 bus bytes per main-loop pass, and not on the pass that works the levels out. The levels are worked out every 10 ms and shown 13 LEDs per pass. An LED is only rewritten when its level changes, or its blink phase if it is blinking (keying every LED on the phase rewrote all 39 every 500 ms: that was most of build 46's 467 us). The TM1628 bit timing is still 1 us per half clock; DIO is open-drain with an unknown pull-up, so faster needs a scope on the line. The pad LED board takes one message per 5 ms whatever the firmware does, so repainting all 16 pads takes ~80 ms.

**Measuring**: the boot report now also has `led update`, `led writes`, `midi out waits` (sends that waited over 100 us for the host to take a 64-byte packet) and the longest wait. All maxima are wall time, so interrupts inside a part count towards it.

