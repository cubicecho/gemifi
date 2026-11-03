# DAW feedback: what's done, and a plan for the rest (#6)

Status 2026-11-03. Draft for review. Background research: [`host-feedback.md`](host-feedback.md). Current behaviour: [`core/README.md`](../../core/README.md) "DAW feedback".

## 1. Where it stands

| Layer | What | State |
|---|---|---|
| Dispatcher | Incoming channel messages and realtime reach the core (USB packets with their cable, or a byte stream) | **Merged.** Host-tested, not run on hardware |
| **F1** plain feedback | LEDs follow notes, CCs, the beat or transport; `cc_toggle` and absolute encoders sync from the host; analog pick-up (`takeover: pickup`); LED ownership (`owner:`, `led_timeout`) | **Merged.** Host-tested, not run on hardware |
| **F2** Mackie Control | Second USB port "`<name>` DAW"; buttons and LEDs, faders as pitch bend, V-Pots and rings, the version reply Live asks for | **Merged, partly.** Missing: LCD text, timecode/assignment display, meters, ring centre LED, fader touch, the connection challenge/response, extender chaining (TODOs in `core/src/mcu.c`) |
| **F3** gemifi feedback SysEx + DAW scripts | Our own host → device messages, plus a Live Remote Script and a Bitwig extension | Not started |
| **F4** MIDI-CI | Discovery and `DeviceInfo` (and maybe controller lists) under the free 0x7C ID | Not started |

The hardware checks for everything merged so far (#7) are parked. **None of this has run in a DAW yet**, and the plan below is written with that in mind.

## 2. Decisions the code already made

The issue's "Still open" list predates the implementation. These are settled unless you want to revisit them:

- **YAML names:** `feedback:` (on the device and on an LED), `takeover: pickup`, `owner:`, `timeout:`, `mcu:`, `ring:`, `usb: { daw_port }`. Not ESPHome-style automations.
- **LED levels for note/CC feedback:** Mackie's 0 off / 1 blink / 2..127 on, for single-colour LEDs. Bicolour and RGB are still open (D2 below).
- **MCU legal note:** `core/README.md` says it's written from community documentation, and the challenge/response isn't implemented (Live doesn't use it).
- **Command numbers:** the `0x0B`–`0x0E` that `host-feedback.md` §5.3 proposed for FB_* are now the PRESET_* commands. F3 needs a new range (proposal in §4).

## 3. Decisions for you

| # | Question | Recommendation |
|---|---|---|
| D1 | **Order:** more feedback layers before the merged ones have run in a DAW? | **No.** Run #7's DAW feedback checks in Live first (an hour with the Keystation). Everything below builds on the dispatcher and LED ownership, so a bug there is cheaper to find now. Phase A can go ahead in parallel because it's docs and spec |
| D2 | **Bicolour and RGB LED levels** | Keep 0/1/2..127 for single-colour LEDs. For bicolour: 0 off, 1 blink colour 1, 2..63 colour 1, 64..127 colour 2. RGB: a palette (index → colour) in the YAML, not a fixed vendor one. Decide this only when a device with such LEDs exists |
| D3 | **`led_timeout` default** (2 s, from the DAW-feedback PR's questions) | Keep 2 s, and recheck it in Live during #7. In Live, momentary-button LEDs go back to local 2 s after the host lit them, even while Live still says "on" |
| D4 | **F3 at all, and when** | **Decided 2026-11-03: now**, straight after finishing F2's protocol work (#19, rest in #18). Spec draft: `protocol/sysex.md` §4.6 (#20). Uses `0x7D` as a placeholder ID until #3 |
| D5 | **Fire-and-forget host → device SysEx** (only matters for F3) | When F3 happens: a separate command range that never replies, written into `protocol/sysex.md` as an exception to "every request gets a reply". A script streaming LED states can't wait for acks |
| D6 | **F4 (MIDI-CI) priority** | Low. Today it gets you a device name and controls shown in MIDI 2.0-aware OS tools, and no DAW uses it for feedback. Do it after F2, on ESP32-S3 only (the NUC120's 4 KB of RAM is tight for it) |
| D7 | **Zynthian** (4th target host) | A short research task: find out what Zynthian sends to a generic controller and whether a small `ctrldev` driver would help. No code before that |
| D8 | **Where the spec lives** | Write `protocol/host-feedback.md` now (phase A): the MIDI that gemifi devices accept and send, F1 and MCU, as implemented. `research/` keeps the background |

## 4. Plan

### Phase A: spec and docs (no hardware needed)
1. `protocol/host-feedback.md`: the incoming message routing (main port vs DAW port, channel rules), F1 behaviour per component, LED levels, pick-up, LED ownership, and the MCU subset implemented (messages, note numbers, the version reply), with what's not implemented listed.
2. DAW setup notes per target host: Live (Remote / Sync on the output, MackieControl on the DAW port), Bitwig, REAPER (its MCU surface, and ReaLearn for F1), Zynthian after D7. Put these on the site's get-started page, with each step marked as untested until #7 checks it.
3. Close the stale items in #6, and move the remaining open ones into this note.

### Phase B: run what's merged (needs you, #7)
4. #7's DAW feedback checklist in Live, with the Keystation and its default YAML, then with a test build that has the DAW port. Fix what breaks. Confirm or change D3.

### Phase C: finish Mackie Control where hardware can use it
5. **Ring centre LED:** a `ring_center:` LED id on encoders with `ring:` (bit 6 of the ring CC). Small.
6. **Connection challenge/response:** check whether Bitwig, REAPER, Logic or Cubase need it (they probably answer without it). Implement only if one of the target hosts does.
7. **A display component**, then MCU's displays on it. The Code 61 has a 6-character LED display on its TM1628-class drivers (`analysis/hw-map.md`), so it's the first device that can show anything:
   - first a generic `display:` component (characters at positions; TM1628 driver first, room for an I²C OLED on ESP later);
   - then MCU's 12 display characters (CC 0x40–0x4B, one character each; community docs put the 10 timecode digits on 0x40–0x49 and the 2-character assignment display on 0x4A–0x4B, to confirm against a DAW) on it, with a YAML choice of which to show;
   - LCD text (the 2×56-character strip) only on a display that can hold it; a 6-character display can show a scrolling or selected-track slice at most. Leave that until a real display wants it.
   This depends on the Code 61 running gemifi on hardware, which is parked, so it's the last item in this phase.
8. **Meters** (channel pressure): only with an LED-bar or display component. Skip for now.
9. **Fader touch:** needs touch-sensing faders (none of the current devices have them). Skip.
10. **Extender chaining** (a second 8-channel bank as MCU XT on cable 2): only for a device with more than 8 faders. Skip.

### Phase D: MIDI-CI on ESP32-S3 (after D6)
11. Discovery (MUID, reply to Discovery Inquiry) and Property Exchange `DeviceInfo` under 0x7C, built from the YAML (`friendly_name`, node name, version). ESP32-S3 first, behind a `midi_ci: {}` YAML key.
12. Then possibly `ChCtrlList` / `AllCtrlList` from the YAML's controls, and our web updater as the first thing that reads them.

### F3 (decided: now)
Spec first (`protocol/sysex.md` §4.6, #20): GET_COMPONENT so a script learns the device's controls and their `daw:` roles, EVENT messages for presses by index, and fire-and-forget FB_* notifications (D5) for LEDs, RGB, values, rings and text. Then the core, the host tool, the web updater, and the Live Remote Script (written from scratch, MIT), then Bitwig. The SysEx ID stays `0x7D` until #3.

## 5. Size and effort

| Item | Device cost (est.) | Effort |
|---|---|---|
| Phase A docs | – | a session |
| Phase B (Live checks + fixes) | – | an hour on hardware, plus fixes |
| Ring centre LED | < 50 B | small |
| Challenge/response | ~200 B | small, if needed at all |
| Display component + TM1628 driver | ~0.5–1 KB | medium; needs the Code 61 on hardware |
| MCU timecode/assignment on it | ~200 B | small, after the display |
| MIDI-CI Discovery + DeviceInfo (ESP32-S3) | 2–4 KB | medium |
| F3 + Live script | ~0.5–1 KB device (+ component names), a Python script to maintain | large; in progress (#20) |

Device numbers are estimates in the style of `host-feedback.md` §5.4, not measurements.
