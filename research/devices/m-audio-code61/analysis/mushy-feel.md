# Code 49 v1.2.1: why the controls feel mushy

Static analysis of `firmware/app_v1.2.1_0x08002400.bin`. All addresses are flash addresses. The pad scaling (0x08009EA0), key offset (0x080121E0-0x08012202) and fader deadband (0x08002A2A) were checked independently against the disassembly. Everything else comes from one analysis pass.

## Which code uses which table

| Function | Called from | Job |
|---|---|---|
| 0x080097D0 | main dispatcher 0x0800C4D2 | **Keys**: velocity = bank A, offset `(curve-1)*128` |
| 0x08009E48 | 0x0800C4C8 | **Pads**: velocity = bank B, its own curve setting |
| 0x0800B7D8 / 0x0800B8EC | second dispatcher 0x0800C868/72 | another mode: keys and pads, always curve0, sends raw USB-MIDI packets |
| 0x08002854 | main loop 0x080105CA | ADC post-processing (8:1 mux; table 0x08014C08 maps mux slots to pads) |
| 0x08011F90 / 0x08012144 | main loop | key-matrix scan, then key timing (0x08014C18 maps scan bits to keys) |

The curve settings are in RAM: keys use 0x20003CA0 and pads use 0x20003CA1. Their reset defaults are at flash 0x080155C0 (`00 01 00 00 00 05 05 0c 0c 02 02 00 02 …`), and EEPROM settings override them. The default is 2, which means curve1, for both.

Timebase: SysTick reload = HCLK/40000, but the CLKSOURCE bit is never set. With HCLK at 72 MHz, the firmware counts five ticks as 1 ms (0x08012A80 `cmp r0,#5`), so **one tick = 200 µs**. The key timestamp at 0x20003826 advances every 2 ticks (**400 µs**).

## Causes, ranked

### 1. Pads: needs ~95% of full scale for 127, measured from slow, late samples (high confidence)

```
08009EA0 cmp.w r1,#0xf20        ; raw >= 3872 → 127
08009EB2 subs r1,r1,r3 ; lsls r5,r1,#4 ; rsb r1,r3,#0xf20 ; asrs r1,#3 ; sdiv
                                ; v = (raw-thr)*128/(3872-thr)
08009ECE rsb r1,r1,#0x7f        ; idx = 127-v → bankB[(curve-1)*128+idx]
08009EF0 cmp r3,#1 ; bls        ; note-on only on the 3rd sample, velocity = max of 3
```

- The default threshold is 0x250 (592). With curve1, velocity ≥100 needs raw ≥ ~3436 (84%) and velocity 64 needs ~2640 (65%). A pressure sensor in a voltage divider rarely gets near the rail.
- Each pad is read once per full 8-step mux cycle. That is 16 ADC frames, each started by software from the main loop.
- The note fires 2 mux cycles after the threshold is crossed, and the 3-sample peak can miss the real peak.
- A pad only re-arms after raw stays ≤0x200 for 3 cycles (0x08009E82), so fast re-strikes get dropped.

### 2. Keys: a near-linear time curve plus an offset that maxes out loud playing (high confidence)

```
080121CE subs r5,r5,r6                   ; dt = t2 - t1 (400 µs ticks)
080121F4 cmp r5,#0x8b ... subs r5,#0xc   ; white keys: idx = min(dt,139) - 12
080121E0 cmp r5,#0x89 ... subs r5,#0xa   ; black keys: idx = min(dt,137) - 10
0800984E ldrb r1,[r1,#9] ; subs r1,#1 ; add r1,r5,r1,lsl#7 ; ldrb r1,[0x08014C58,r1]
```

- Velocity is 127 for any two-contact time ≤6.0 ms (white) or ≤5.2 ms (black). Real fortissimo is often 2-4 ms.
- At the loud end, one 400 µs tick is about 2-4 velocity steps.
- Curve1 falls about 1 velocity per index, so velocity is linear in *time* and barely changes with key *speed* at the fast end.
- Window: 4.8-55.6 ms. That gives 100 at 11.2 ms, 64 at 23.6 ms, 32 at 36 ms.
- Curve0 sits at 8 for 15 indices (98-112). Curves 4-6 are flat 64 / 100 / 127.
- Inference: scan timing jitter equals the main-loop period. That can't be measured statically.

### 3. Faders: large deadband when idle, plus block averaging (high confidence)

```
08002A2A movs r4,#0x20 / 08002A2E movs r4,#0xa0  ; deadband 32 while active, 160 when idle
08002A42 mov.w r4,#0x1f40                        ; active window = 8000 mux steps
0800A0FE subs r2,#0x50 ; muls ; udiv #0xf50      ; 12-bit → CC, 30.6 counts per step
```

- From rest, a fader has to move ~5 CC steps before anything is sent, then the value jumps.
- Each value is a 4-sample average. Faders 1-8 are muxed, so a new value arrives only every 4 mux cycles.

### 4. Encoders: acceleration on by default, reversals dropped (high confidence)

```
08009556 ldrb r8,[0x20003c97,#0xc]   ; acceleration mode, default 2
08009610 dt<15ms → ±7, <21 → ±5, <31 → ±3, else ±1
0800957A cmp r0,#0x50 ; … bne drop   ; first step after a reversal within 80 ms is dropped
```

Decoding is ×1, one step per rising edge of phase A (0x08012310).

### 5. Wheels and aftertouch

- Pitch and mod: 5-sample average with ±10-count hysteresis (0x08002896 / 0x080028DC). The default span is 0x620-0x950 (816 counts), about 6.4 counts per step, so the hysteresis forces ~2-step jumps.
- Aftertouch: the window 0x4D0-0x900, inverted (0x080087F0). It is muted for 60 ms after the first key goes down (0x08012218 sets `[0x46]=60`, counting down once per ms).

## Candidate patches (untested)

The bootloader may checksum the app, and the update-block checksum is still unknown.

| What | Where | Change |
|---|---|---|
| Key curve | bank A curve1 (0x08014CD8) | log-of-time curve: `127,123,119,116,112,109,106,103,101,98,96,94,91,89,87,85,…,3,3,2,2,1,1` |
| Key offset (white) | 0x080121FE | `0c3d` → `063d` (subtract 6) |
| Key offset (black) | 0x080121EA | `0a3d` → `053d` |
| Pad curve | bank B (0x08014FD8+) | boost, e.g. `127*sqrt(v/127)` |
| Pad full scale | 0x08009EA0, 0x08009EB6 | 0xF20 → ~0xC00 (Thumb-2 modified-immediate) |
| Pad fire on 2nd sample | 0x08009EF0 | `012b` → `002b` |
| Fader idle deadband | 0x08002A2E, 0x08002A94 | `a024` → `4024` (64) |
| Encoder acceleration | 0x080155CC | `02` → `00` (reset default only; EEPROM wins) |
| Reversal lockout | 0x0800957A | `5028` → `1028` (16 ms) |
| Wheel hysteresis | 0x08002896, 0x0800289E, 0x080028DC, 0x080028E4 | `#0xa` → `#4` |

## Needs hardware to settle

1. Main-loop period: GPIO toggle, or scope the mux selects PC0-PC2 (one mux step = 2 loop passes).
2. Pad sensor voltage on a hard hit (ch0/ch1): is 3872 counts reachable?
3. Real two-contact times on the key matrix (GPIOB 8-15) for pp and ff.
4. Per-unit EEPROM calibration and thresholds (the 0x35-byte block), which override these defaults.
