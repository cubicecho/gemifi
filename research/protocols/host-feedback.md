# Host → device feedback: protocols and a plan for gemifi

Research for [#10](https://github.com/cubicecho/gemifi-private/issues/10): how a DAW or other host can send state *back* to a gemifi controller (LEDs, rings, fader positions, text, meters, transport, tempo), which existing specs to use, and what each would cost on the NUC120 (32 KB flash, 4 KB RAM) and the ESP32 family.

Status: research only, 2026-11-01. Nothing here is implemented. The issue asks for a design note in `protocol/`; that should follow once the maintainer has picked from the options below (see "Open questions"). "machui" in the issue is read as **Mackie Control / HUI** (both are Mackie protocols, and the issue's own table lists them); NI Maschine/NKS is covered briefly in §4.4 in case that was meant.

---

## 0. Summary

| Layer | What it gives | Host support | NUC120 cost (estimate) | Recommendation |
|---|---|---|---|---|
| **A. Plain MIDI echo** (CC/note/program sent back by the host; MIDI clock, Start/Stop) | LEDs follow DAW state; toggles and absolute encoders stay in sync; pots get pick-up; beat/play LEDs | Every DAW that can send MIDI to a mapped controller (Live: "Remote" output switch) | ≈ 0.6–1 KB flash, ≈ 10–40 B RAM | **Do first.** |
| **B. Mackie Control (MCU) mode**, on a *second USB-MIDI cable* | Transport, rec/solo/mute/select LEDs, faders (pitch bend), V-pots and rings, meters, optional LCD/timecode | Built in to Live, Logic, Cubase, Reaper, Bitwig, FL Studio, Studio One (§1.4) | ≈ 1.5–2.5 KB flash, ≈ 30–60 B RAM without a display (+112 B for a 2×56 LCD buffer) | **Second.** Zero host setup beyond picking "Mackie Control". |
| **C. gemifi SysEx feedback commands** (+ our own Live Remote Script / Bitwig extension) | Anything: names, colours, values with labels, layouts | Only where we write host code | ≈ 0.3–0.5 KB flash device side; most work is host scripts | **Third**, when a device has a display or RGB LEDs. |
| **D. MIDI-CI** (Discovery + Property Exchange responder) | Standard discovery and self-description (DeviceInfo, AllCtrlList names); not a feedback channel in practice yet | OS APIs exist (Windows MIDI Services, CoreMIDI); DAW use is thin and mostly unverified | ≈ 2–4 KB flash, ≈ 0.1–0.3 KB RAM (GET only, JSON in flash) | **Later**; ESP first. Not a feedback path today. |
| HUI | Pro Tools surface | Pro Tools (and Logic) | similar to MCU plus a ping/keep-alive | Skip unless someone needs Pro Tools. |

Two findings change assumptions in the issue:

1. **The NUC firmware drops every non-SysEx message it receives today.** `platform/nuvoton-nuc1xx/src/usb_midi.c` only feeds USB-MIDI CINs 4–7 (SysEx) into the SysEx parser; Note, CC, Pitch Bend, Clock (CIN 0x8–0xF) are discarded. On ESP, `gm_midi_rx_byte()` passes every byte to `app_proto_rx_byte()`, which also only understands SysEx. Any feedback work starts with a small channel/realtime message dispatcher in the core.
2. **A second "DAW port" does not need extra USB endpoints.** USB-MIDI 1.0 multiplexes up to 16 virtual cables over one bulk IN/OUT endpoint pair: the high nibble of byte 0 of every 4-byte USB-MIDI event packet is the cable number, and each embedded jack is associated with the endpoint in its class-specific descriptor ([USB MIDI 1.0 class spec, §4 and §6.2](https://ftp.icm.edu.pl/Linux/alsa/manuals/usb/midi10.pdf); our own `usb_midi.c` already writes `cable << 4 | CIN`). A second port costs about 40–60 bytes of descriptors and some routing code, not endpoints. The OS shows it as a second MIDI port, which is what Novation's "DAW" ports are. This matters because MCU on the *same* port as the keys does not work in Live (§1.5).

---

## 1. Mackie Control (MCU), Logic Control and HUI

### 1.1 Status of the spec

- Mackie has never published the MCU protocol; it was licensed to software vendors, and public knowledge comes from the **Logic Control** documentation (Emagic, later included as "Logic Control — MIDI Implementation" in Apple's Logic 7 manual, Appendix B) plus reverse engineering ([EMATech TouchMCU protocol notes](https://github.com/EMATech/TouchMCU/blob/main/doc/mackie_control_protocol.md); [Logic 7 manual appendix, p. 239 (manualsdir mirror)](https://www.manualsdir.com/manuals/547263/apple-logic-7.html?page=239); [linux-audio-dev thread, 2015](https://lists.linuxaudio.org/archives/linux-audio-dev/2015-July/036017.html)). The appendix itself says it is aimed at software vendors integrating with Logic/Mackie Control units.
- Many vendors ship MCU-compatible surfaces (Behringer X-Touch, iCON, PreSonus FaderPort, Icon Platform M+, etc.), and DAWs expose a generic "Mackie Control" setting for them ([iCON DAW docs](https://support.iconproaudio.com/hc/en-us/articles/5855576840205); [PreSonus ATOM SQ / HUI](https://support.presonus.com/hc/en-us/articles/360046164932-ATOM-SQ-How-to-set-up-as-a-HUI-device-in-Pro-Tools)).
- **Can an MIT project implement it?** Implementing a wire protocol for interoperability, written from community documentation, is what all those vendors and open projects (e.g. [TouchMCU, GPL-3.0](https://github.com/EMATech/TouchMCU); MIDIbox LC) do. We should write our own tables from the message formats (facts), not copy text from the Logic manual or code from Ableton's scripts, and not use "Mackie" in product names beyond describing compatibility. This is not legal advice; see Open questions.

### 1.2 Message summary (MCU, device ID `0x14`; XT extender `0x15`)

From the TouchMCU notes unless marked. "Ch 1" = MIDI channel 1 (status nibble 0).

| Element | Device → host | Host → device (feedback) |
|---|---|---|
| Buttons / LEDs | Note On ch 1, vel `7F` press, `00` release | Note On ch 1 same note: `00` off, `01` blink, `7F` on |
| Rec / Solo / Mute / Select ×8 | notes `00–07`, `08–0F`, `10–17`, `18–1F` | same notes (LEDs) |
| V-pot push ×8 | notes `20–27` | – |
| Assign: Track, Send, Pan, Plug-in, EQ, Instrument | `28–2D` | LEDs |
| Bank ◀▶, Channel ◀▶ | `2E 2F`, `30 31` | – |
| Flip, Global, Name/Value, SMPTE/Beats | `32–35` | LEDs |
| F1–F8 | `36–3D` | – |
| Marker, Nudge, Cycle, …, Click | `54`, `55`, `56`, …, `59` | LEDs |
| Transport ⏪ ⏩ ■ ▶ ● | `5B 5C 5D 5E 5F` | LEDs (play/record state) |
| Cursor ▲▼◀▶, Zoom, Scrub | `60–63`, `64`, `65` | LEDs |
| Fader touch ×8, master | notes `68–6F`, `70` | – |
| Faders ×8, master | Pitch Bend ch 1–8, ch 9 (14-bit) | **Same Pitch Bend** (motor/LED position) |
| V-pot rotation ×8 | CC `10–17`: `01..` = +n, `41..` = −n | – |
| V-pot LED ring ×8 | – | CC `30–37`, value `0 c mm vvvv`: c = centre LED, mm = mode (dot / boost-cut / wrap / spread), vvvv = 0–11 |
| Jog wheel | CC `3C`, same relative encoding | – |
| Timecode / assignment 7-seg (12 digits) | – | CC `40–4B`, one character per CC (bit 6 = decimal point) |
| Meters ×8 | – | Channel Pressure ch 1, data `s v`: s = strip 0–7, v = level 0–C (D/E/F = overload set/clear) |
| LCD 2×56 | – | SysEx `F0 00 00 66 14 12 pos chars… F7`, pos `00–37` top line, `38–6F` bottom |
| Version | reply `F0 00 00 66 14 14 <5 ASCII> F7` | request `F0 00 00 66 14 13 00 F7` |
| Handshake (optional?) | `01` host-connection query (serial + challenge), `03` confirm | `00` device query, `02` reply |
| Meter mode | – | `F0 00 00 66 14 20 strip mode F7`, `…21 h/v F7` |

Ableton Live's MackieControl script requests the firmware version with `F0 00 00 66 14 13 00 F7` and accepts the `…14 14 …` reply; it does not do the challenge/response, and it sends meters as Channel Pressure and the LCD as SysEx `…12` (observed in the decompiled Live 12 scripts, [gluon/AbletonLive12_MIDIRemoteScripts `MackieControl/`](https://github.com/gluon/AbletonLive12_MIDIRemoteScripts); read for interoperability only, nothing copied). Whether Logic, Cubase or others insist on the challenge/response was **not verified**; community emulators (TouchOSC/Lemur layouts) suggest most hosts work without it ([MIDIkinetics forum](https://community.midikinetics.com/viewtopic.php?p=11865)).

### 1.3 HUI

HUI (device ID `0x05`) uses a zone/port scheme for buttons and LEDs, a ping that the device must answer (Note On 0 vel 0 → vel 7F), and its own meter/timecode messages. It was never published; the only detailed source is a reverse-engineered write-up whose copies disagree on details such as whether missing pings take the unit offline ([HUI protocol PDF, manualmachine mirror](https://manualmachine.com/mackie/hui/27244700-hui-midi-protocol); [MIDIbox forum](https://forum.midibox.org/t/where-is-the-mackie-hui-protocol-spec/8579)). Pro Tools is the main reason to support it ([Avid EUCON setup guide, HUI appendix](https://manualmachine.com/avidtechnology/eucon/19732005-application-setup-manual)); Logic also supports HUI ([Apple Logic Pro Control Surfaces guide](https://support.apple.com/guide/logicpro-css/mackie-control-overview-ctls22c3ba7b/mac)). **Recommendation: skip for now.**

### 1.4 DAW support for MCU

| DAW | MCU | Source | Notes |
|---|---|---|---|
| Ableton Live | ✓ built-in "MackieControl", "MackieControlXT" | [decompiled script list](https://github.com/gluon/AbletonLive12_MIDIRemoteScripts); [Live manual §34.1.1](https://www.ableton.com/en/live-manual/12/midi-and-key-remote-control/) (control surface chooser) | Up to six surfaces at once |
| Logic Pro | ✓ (and HUI) | [Apple Logic Pro Control Surfaces Support Guide](https://support.apple.com/guide/logicpro-css/mackie-control-overview-ctls22c3ba7b/mac) | Native home of Logic Control |
| Cubase / Nuendo | ✓ "Mackie Control" remote device | [Steinberg Cubase 9 docs](https://archive.steinberg.help/cubase_pro_artist/v9/en/cubase_nuendo/topics/remote_control/remote_control_remote_device_selecting_t.html) | Current versions not re-checked |
| REAPER | ✓ "Mackie Control Universal" surface mode; also CSI | [Neyrinck V-Control REAPER setup](https://docs.neyrinck.com/v-control-pro/reaper/); [CSI docs](https://hoard.reaperaccessibility.com/Documentation%20and%20Product%20Layouts/csi%20doc.txt) | Third-party sources |
| Bitwig Studio | ✓ "Mackie Control" + 1–3 extenders bundled | [Bitwig supported controllers](https://bitwig.com/support/technical_support/which-controllers-are-supported-in-bitwig-studio-28) | Also DrivenByMoss (LGPL-3.0) |
| FL Studio | ✓ since FL 8 | [Image-Line forum](https://forum.image-line.com/viewtopic.php?p=1357214) | Community source |
| Studio One | ✓ (vendor lists) | [iCON DAW docs](https://support.iconproaudio.com/hc/en-us/articles/5855576840205) | Not checked against PreSonus docs |
| Pro Tools | HUI only | [Avid EUCON guide](https://manualmachine.com/avidtechnology/eucon/19732005-application-setup-manual) | |

### 1.5 Why MCU must be on its own port for a keyboard

MCU uses Note On for buttons regardless of musical meaning (`5E` = Play, `5F` = Record, `00–1F` = rec/solo/mute/select). Live's MackieControl script classifies incoming messages by status nibble only (`midi_bytes[0] & 0xF0`), so key presses on *any* channel arriving on the MCU input would be read as button presses (observed in the decompiled script; not tested on hardware). A keyboard that also offers MCU therefore needs:
- a **second USB-MIDI cable** ("<device> DAW" port) carrying MCU, with keys/pots on cable 0 (Novation does this with separate DAW ports, e.g. [Launchkey MK3 Programmer's Reference](https://fael-downloads-prod.focusrite.com/customer/prod/downloads/launchkey_mk3_programmer_s_reference_guide_v1_en.pdf)); or
- a mode switch (keys stop sending while in MCU mode), which is poor UX for a keyboard.

On serial and BLE MIDI there is only one port; there MCU mode must be a whole-device mode (fine for a fader box with no keys).

---

## 2. MIDI 2.0 / MIDI-CI

### 2.1 What it is

- **MIDI-CI** is a set of Universal SysEx messages (`F0 7E <dev> 0D <sub-ID#2> …`) with 28-bit random MUIDs for addressing. Over a MIDI 1.0 byte-stream connection, device ID `00–0F` addresses a channel, `7E` the whole port and `7F` the "function block" ([M2-101-UM MIDI-CI v1.2, §3.2.1, §3.3](https://amei-music.github.io/midi2.0-docs/amei-pdf/M2-101-UM_v1-2_MIDI-CI_Specification.pdf)). So it **does work over USB-MIDI 1.0**.
- Categories: Profile Configuration (`0x2x`), Property Exchange (`0x3x`), Process Inquiry (`0x4x`), plus mandatory Management messages (`0x7x`: Discovery, Invalidate MUID, ACK/NAK) (ibid. §5.5.2).
- **Discovery reply** carries manufacturer SysEx ID (3 bytes), family, model, software revision, supported categories and **Receivable Maximum SysEx Message Size**; every MIDI-CI device must accept **at least 128 bytes**, and devices that *initiate* Profile or PE transactions at least 512 (ibid. §5.5.3). A MUID must be newly random on every power-up (§3.3.1).
- **Property Exchange (PE)**: JSON header + JSON body inside SysEx, chunked, with GET, SET, Subscribe and Notify (ibid. §8). Standard resources include ResourceList, DeviceInfo, ChannelList ([M2-105](https://amei-music.github.io/midi2.0-docs/amei-pdf/M2-105-UM_v1-1-1_Property_Exchange_Foundational_Resources.pdf)), ProgramList, Mode, LocalOn, ExternalSync, Get/Set Device State, and the **Controller resources AllCtrlList / ChCtrlList / CtrlMapList** ([M2-117](https://amei-music.github.io/midi2.0-docs/amei-pdf/M2-117-UM_v1-0_Property_Exchange_Controller_Resources.pdf)). Manufacturer-specific resources use an `X-` prefix (Common Rules for PE, M2-103).
- AllCtrlList is the closest standard thing to "what controls does this device have": each entry has a required `title`, `channel`, `ctrlType` (cc, chPress, pPress, nrpn, rpn, pBend, …) and optional description, priority, typeHint, ranges. Its stated use cases are a DAW adding a device's controllers to automation and a keyboard auto-assigning its knobs to a synth's most useful controllers (M2-117 §1.1). It describes **what a device sends/recognises**, not live values coming back.
- **Profiles** published (from the AMEI download list): Default Control Change Mapping, Default Drum Note Map, GM2 single-channel and function-block, MPE, Drawbar Organ, Rotary Speaker, Orchestral Articulation. **There is no published DAW-control / mixer / control-surface profile** in that list (checked 2026-11-01 against [AMEI's MIDI 2.0 document site](https://amei-music.github.io/midi2.0-docs/)). Whether one is in MIDI Association working groups could not be verified.
- **Process Inquiry / MIDI Message Report** lets an initiator ask a device to report its current state as ordinary MIDI messages (current CC values etc.) (M2-101 §9). That is host←device (the host learns *our* positions), useful for pick-up, not feedback to us.

### 2.2 Licensing

The specs are free downloads: from midi.org with a free account, and without login from AMEI's site, which forbids selling, editing/redistributing them or bundling misleading supplements ([AMEI MIDI 2.0 docs site](https://amei-music.github.io/midi2.0-docs/ja/)). Each PDF is "© MMA and AMEI, all rights reserved" (M2-117 p. 2). **Implementing them is fine; do not commit the PDFs or long excerpts.** A shipped product needs a real SysEx manufacturer ID in the Discovery reply; gemifi's `0x7D` is for non-commercial/development use only (already noted in `protocol/sysex.md` §1).

### 2.3 OS and DAW support (Nov 2026)

| Platform | MIDI-CI | Source |
|---|---|---|
| Windows | Windows MIDI Services GA Feb 2026 (in-box, phased rollout); WinRT `Windows.Devices.Midi2.CapabilityInquiry` namespace with PE helpers (DeviceInfo, ResourceList, ChannelList, ProgramList, subscriptions), UMP-based | [Windows blog 2026-02-17](https://blogs.windows.com/windowsexperience/2026/02/17/making-music-with-midi-just-got-a-real-boost-in-windows-11/); [SDK reference](https://microsoft.github.io/MIDI/sdk-reference/CapabilityInquiry/) |
| macOS / iOS | CoreMIDI has had UMP since macOS 11; `MIDICIDiscoveryManager`, `MIDICIResponder` APIs exist; their status on macOS 15+ looks deprecated (unverified) | [Apple MIDICIDiscoveryManager](https://developer.apple.com/documentation/coremidi/midicidiscoverymanager); [Apple MIDICIResponder](https://developer.apple.com/documentation/coremidi/midiciresponder) |
| Linux | ALSA UMP since kernel 6.5; userspace via alsa-lib/alsa-utils; MIDI-CI itself is left to applications | [kernel docs, MIDI 2.0 on Linux](https://docs.kernel.org/6.6/sound/designs/midi-2.0.html) |
| DAWs | Only secondary claims: Logic with PE auto-mapping, Studio One with Discovery, Cubase with MIDI 2.0→VST3, Ableton and FL nothing shipped. **Not verified** against vendor docs | [midi2kit blog, 2026-03](https://midi2kit-github-io.pages.dev/blog/2026-03-08-midi2-ecosystem-2026.html) |
| Hardware | KORG Keystage cited as the first controller shipping PE | same blog; note Live 12 has a `Keystage` Remote Script ([gluon list](https://github.com/gluon/AbletonLive12_MIDIRemoteScripts)) |

**Conclusion:** MIDI-CI is the right long-term way to *describe* a gemifi device (names, controller list, maybe a gemifi `X-` resource carrying our schema), and it is cheap enough as a GET-only responder. It is **not** a practical feedback path today: no DAW drives LEDs or rings through PE, and no profile for control surfaces exists.

### 2.4 Feasibility on the NUC120

A responder that answers Discovery, PE Capabilities and GET for ResourceList, DeviceInfo and AllCtrlList:
- JSON bodies generated by the builder into flash (a 7-button/1-pot Keystation AllCtrlList is roughly 0.5–1 KB of JSON), streamed out in chunks no larger than the host's max SysEx size, so no RAM copy.
- Receive: the current `rx_buf` (≈ 310 B) already exceeds the 128-byte minimum. Header parsing is a string match for `"resource":"…"` and `"resID"`.
- RAM: MUID, request ID, chunk cursor: well under 100 B. SET and Subscribe need more state and are optional.
- Flash: ≈ 2–4 KB including the JSON. Fits (the app is 12.7 KB of 32 KB today, `firmware/nuvoton-nuc1xx/midi-controller/README.md`), but it competes with MCU and future features. ESP has no such limit.
- MUID needs a random source at boot: the NUC has no TRNG; use the stored random UID mixed with an ADC-noise/timer sample (the spec wants a new MUID every power-up).

---

## 3. Ableton Live

### 3.1 How Live talks to controllers

- **Control Surface chooser** (Preferences → Link/Tempo/MIDI): picks a *MIDI Remote Script* for a named controller, up to six at once; scripts provide "instant mappings" that follow the selected device ([Live 12 manual §34.1.1](https://www.ableton.com/en/live-manual/12/midi-and-key-remote-control/)).
- **Manual mapping** (MIDI Map mode): any CC/note to any parameter. For feedback, the user must enable **Remote** on the *output* port; Live then sends value changes back "to update motorized faders and LEDs" (ibid. §34.1.2). **Takeover** modes None / Pick-Up / Value Scaling handle absolute controls that disagree with the parameter (ibid. §34.1.3). This is layer A and costs us nothing on the host side.
- **Remote Scripts** are Python 3 since Live 11; third-party scripts go in `User Library/Remote Scripts` since Live 10.1.13; Ableton does not support third-party scripts ([Ableton: Installing third-party remote scripts](https://help.ableton.com/hc/en-us/articles/209072009)). The framework (`_Framework`, `ableton.v2`, `ableton.v3`) is undocumented; developers read the decompiled scripts ([gluon/AbletonLive12_MIDIRemoteScripts](https://github.com/gluon/AbletonLive12_MIDIRemoteScripts), no licence: © Ableton) or community stubs ([loftkid/ableton-remote-script-docs](https://github.com/loftkid/ableton-remote-script-docs)).
- The older **UserConfiguration.txt** "user remote script" (a text template mapping a few CCs to device/track/transport functions) still appears in vendor guides ([Akai MPK Mini Plus](https://support.akaipro.com/support/solutions/articles/69000827950-akai-pro-mpk-mini-plus-ableton-remote-script-beta-)); it gives no LED feedback beyond what Live sends for mappings. Its status in Live 12 was not verified.
- **Push 2**: Ableton published the full MIDI + display interface manual ([Ableton/push-interface](https://github.com/Ableton/push-interface), no licence file: documentation, not open source). A good reference for LED colour palettes and pad/encoder feedback design.

### 3.2 Practical paths for a gemifi device in Live

| Path | Host work | Feedback you get | Verdict |
|---|---|---|---|
| Manual mapping + Remote output | User maps and enables Remote | Values of mapped params as CC/note | Works now once the device listens (layer A) |
| Select "MackieControl" on the DAW port | None | Full MCU set (§1.2): transport, track buttons, faders, V-pots, meters, LCD | Layer B |
| Own Remote Script ("gemifi") | Write and maintain a Python 3 script; users install it | Anything, incl. our SysEx (track names, colours, clip states) | Layer C; must be written from scratch (MIT), never derived from decompiled code |

Licensing: our Remote Script may *call* Live's Python API, but must not copy decompiled Ableton code into the repo. Ableton may change the framework between releases.

---

## 4. Other hosts and approaches

### 4.1 Bitwig Studio
Open, documented Controller/Extension API in Java or JavaScript; Bitwig's own extensions are MIT ([bitwig/bitwig-extensions](https://github.com/bitwig/bitwig-extensions)); bundled "Generic" scripts (MIDI keyboard, keyboard + 8 knobs CC 20–27) and Mackie Control ([Bitwig supported controllers](https://bitwig.com/support/technical_support/which-controllers-are-supported-in-bitwig-studio-28)). [DrivenByMoss](https://github.com/git-moss/DrivenByMoss) (LGPL-3.0) supports MCU and many surfaces with rich feedback. A gemifi extension (layer C) is straightforward here. Whether Bitwig's generic MIDI mappings send value feedback was **not verified**.

### 4.2 REAPER
- Built-in "Mackie Control Universal" surface mode (§1.4).
- **CSI** (Control Surface Integrator), open source, maps any surface (incl. MCU and OSC) with "feedback processors" that send state back; REAPER owns button state ([CSI documentation](https://hoard.reaperaccessibility.com/Documentation%20and%20Product%20Layouts/csi%20doc.txt); [CSI code](https://github.com/benvenutti/CSICode)).
- **ReaLearn** ([helgoboss/helgobox](https://github.com/helgoboss/helgobox), GPL-3.0): per-mapping MIDI/OSC feedback, pick-up; the easiest way to drive layer-A LEDs from REAPER.

### 4.3 OSC and network
OSC is open and well supported (REAPER, Bitwig via DrivenByMoss, [Open Stage Control](https://github.com/jean-emmanuel/open-stage-control)), but only usable by network (ESP32 Wi-Fi) builds, not USB-MIDI. Ableton Link (tempo/phase over the LAN) is open source (GPLv2+ or proprietary licence, per the issue) and also network-only. Out of scope for the NUC; possible ESP extras.

### 4.4 Vendor-specific (reference only)
- **Novation**: separate DAW port, DAW-mode enable message, LED colours by note/CC velocity palette or SysEx RGB ([Launchkey MK3 Programmer's Reference](https://fael-downloads-prod.focusrite.com/customer/prod/downloads/launchkey_mk3_programmer_s_reference_guide_v1_en.pdf); [Launchpad Pro MK3 reference](https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/LPP3_prog_ref_guide_200415.pdf)). A good model for gemifi's own LED palette messages.
- **Native Instruments NKS / NIHIA**: proprietary. Since NAMM 2025 NI runs an NKS Hardware Partner Program for named manufacturers (Akai, Novation, Nektar, Korg, M-Audio), starting with MIDI-mapping-level integration ([NI blog](https://blog.native-instruments.com/nks-hardware-partner-program/)). Terms are not public; not an option for an open project.

### 4.5 Sync and transport (any host)
MIDI Clock (`F8`, 24 per quarter note), Start/Continue/Stop (`FA FB FC`) and Song Position Pointer (`F2`) are MIDI 1.0 System Real Time/Common messages every DAW can send to an output port. On USB-MIDI they arrive as single-byte CIN `0xF` (realtime) or CIN `0x2/0x3` (SPP). They give a tempo LED and a play LED with no protocol work. MTC is not needed for LEDs.

---

## 5. Recommendation for gemifi

### 5.1 Layered plan

1. **Milestone F1 – listen (layer A + clock).** Core gets a MIDI input dispatcher (Note, CC, Program, Pitch Bend, Channel Pressure, realtime). Device YAML lets an LED follow a host message; button LEDs and `cc_toggle` state sync with the host's echo of the button's own CC by default; absolute encoders adopt host values; pots get optional pick-up; LEDs can show clock beat and play state. First test: Keystation `edit` (CC 80 toggle) LED following a Live mapping with Remote on, octave LEDs blinking the beat.
2. **Milestone F2 – second USB-MIDI cable + MCU subset (layer B).** "<name> DAW" port on USB devices. YAML `mcu:` section; buttons can have `action: mcu` with `mcu: play`; faders send pitch bend and follow it; encoders send V-pot deltas and drive ring LEDs; meters and an LCD only when the device declares hardware for them. Answer the version request. Bank of 8 only (no XT chaining at first).
3. **Milestone F3 – gemifi feedback SysEx (layer C).** A few host→device commands in `protocol/sysex.md` (proposed below) plus a gemifi Live Remote Script and Bitwig extension, for devices with displays or RGB LEDs. The web updater can use the same commands to test LEDs.
4. **Milestone F4 – MIDI-CI responder (layer D).** Discovery + PE GET for DeviceInfo, ResourceList, AllCtrlList (+ an `X-gemifi-schema` resource that wraps our existing schema), ESP first, NUC if flash allows.

### 5.2 Device YAML sketches (illustrative, not final)

Layer A: an LED follows a host message; buttons follow their own echo.

```yaml
midi:
  transport: usb
  feedback: true              # listen to the host at all (default true on USB)

buttons:
  - id: edit
    pin: { number: PA2, inverted: true }
    action: cc_toggle
    cc: 80
    led: edit_led
    sync_from_host: true      # default: a CC 80 from the host sets the toggle state (and so the LED)

leds:
  - id: edit_led
    pin: { number: PA13, inverted: true }
  - id: play_led
    pin: { sn74hc595: leds, number: 4 }
    source: { transport: playing }        # Start/Continue → on, Stop → off
  - id: beat_led
    pin: { sn74hc595: leds, number: 5 }
    source: { clock: beat, length: 50ms } # flash on every quarter note
  - id: arm_led
    pin: { sn74hc595: leds, number: 6 }
    source: { note: 36, channel: 16 }     # velocity 0 = off, 1..63 = blink, 64..127 = on (palette TBD)
    timeout: 10s                          # back to local state if the host goes quiet (optional)

analog:
  - id: volume
    pin: PA0
    cc: 7
    takeover: pickup          # none | pickup: ignore the knob until it crosses the host's last value

encoders:
  - id: encoder_1
    pin_a: { number: GPIO20, mode: INPUT_PULLUP }
    pin_b: { number: GPIO21, mode: INPUT_PULLUP }
    cc: 30                    # absolute mode: a CC 30 from the host becomes the encoder's value
```

Layer B: MCU on a second cable.

```yaml
usb:
  vid: 0x1209
  pid: 0x0002
  ports: [main, daw]          # second USB-MIDI cable, shown as "<friendly_name> DAW"

mcu:
  port: daw
  device: mcu                 # mcu (0x14) | mcu_xt (0x15)
  first_strip: 1              # which of the 8 strips this device's strip controls map to

buttons:
  - { id: play,   pin: PA5, action: mcu, mcu: play,   led: play_led }   # note 0x5E, LED from host
  - { id: rec_1,  pin: PA6, action: mcu, mcu: rec_1,  led: rec_1_led }  # note 0x00
analog:
  - { id: fader_1, pin: { cd74hc4051: faders, number: 0 }, mcu: fader_1 }  # pitch bend ch 1; motor/LEDs follow
encoders:
  - { id: pan_1, pin_a: GPIO20, pin_b: GPIO21, mcu: vpot_1, ring: ring_1 }  # CC 0x10 out, CC 0x30 in
led_rings:                                  # new component: 11–13 LEDs driven from an MCU ring value
  - id: ring_1
    pins: [ { sn74hc595: rings, number: 0 }, … ]
```

Names (`play`, `rec_1`, `vpot_1`, …) would be a fixed table in the builder that maps to MCU note/CC numbers, validated like the button actions. The builder should reject a `mcu:` section on a single-port transport (serial/BLE) unless the device has no keys and no other controls on cable 0.

### 5.3 Proposed gemifi SysEx additions (layer C)

Host → device, no reply needed (fire-and-forget, so a script can stream them); new request numbers in the free `0x0B–0x0E` range, or a new range if we want unacknowledged host messages kept apart:

| cmd | Name | Payload |
|---|---|---|
| `0x0B` | FB_LED | `index` u14, `level` (0 off, 1 on, 2 colour 2, 3 both, 4+ blink…; same levels as `led_show()`) |
| `0x0C` | FB_VALUE | `kind` (1 analog, 2 encoder, 3 ring), `index` u14, `value` u14 |
| `0x0D` | FB_TEXT | `display` u14, `pos` u14, text (7-bit ASCII) |
| `0x0E` | FB_RELEASE | – (hand every LED back to local state) |

These reuse `gsx` framing, so the cost is a few switch cases (~300–500 B flash). The open question is whether host→device messages should be unacknowledged (protocol rule today: every request gets a reply).

### 5.4 Cost estimates (NUC120, Keystation-size device)

Rough, from the size of similar code in `core/src/controller.c`; to be measured.

| Feature | Flash | RAM | Notes |
|---|---|---|---|
| RX dispatcher (USB CIN 0x8–0xF on NUC; status/running-status parser for serial/BLE on ESP) | 250–400 B | 4–8 B | Required by everything below |
| LED `source:` table + host level per LED | 150–300 B (+3–4 B per LED in tables) | 1 B per LED | |
| `sync_from_host` for toggles, encoder adopt, analog pick-up | 150–300 B | 1 B per analog (last host value) | |
| Clock/transport LEDs | 100–200 B | 4 B | |
| Second USB-MIDI cable | 60–100 B descriptors + routing | 0 | Needs USB descriptor rework and a hardware test on Windows/macOS/Linux |
| MCU mode (buttons, LEDs, faders, V-pots, rings, meters, version reply) | 1.2–2 KB | 20–40 B | + 112 B for an LCD buffer, 12 B for timecode digits, only if declared |
| gemifi FB_* SysEx | 300–500 B | 0 | |
| MIDI-CI GET-only responder | 2–4 KB incl. JSON | 60–100 B | |
| **Total, all layers** | ≈ 5–8 KB | ≈ 0.1–0.3 KB | App is 12.7 KB / 1.7 KB today, so it fits, but leaves less room for later features |

### 5.5 Host-side work

| Layer | Host work |
|---|---|
| A | None for us. Users enable Remote output (Live), feedback in ReaLearn/CSI (REAPER), or the DAW's equivalent. Document per DAW. |
| B | None for us. Users pick "Mackie Control" on the DAW port. Document per DAW, test Live/Bitwig/REAPER/Logic. |
| C | Write and maintain: Live Remote Script (Python 3, from scratch, MIT), Bitwig extension (Java, MIT API), optionally REAPER via CSI/ReaLearn configs; web updater LED test page. |
| D | Little on the device side; useful once DAWs consume PE. Our own web tool could read DeviceInfo/AllCtrlList as a first consumer. |

---

## 6. Open questions for the maintainer

1. **Which DAWs first?** Live is assumed; is REAPER, Bitwig or Logic also a priority? That decides whether layer C (scripts) is worth it soon.
2. **Second USB-MIDI cable on the Keystation**: OK to change the USB descriptor (port name "<name> DAW")? The issue assumed it needed extra endpoints; it does not, but it does change how the device appears in every OS and DAW and needs a hardware test.
3. **MCU on the Keystation at all?** It has 7 buttons and 4 LEDs and no faders/encoders; only transport (play/stop/record) with LEDs is really useful. Is that enough for F2, or should MCU wait for the ESP32-S3 fader box / Code 61?
4. **Legal comfort with MCU**: implementing the protocol from community docs is common practice, but does the project want a note in the README, and should it avoid the (optional) challenge/response handshake entirely?
5. **LED ownership rule**: when both the host and local logic want an LED (e.g. octave LEDs), who wins? Proposal: host wins while it has sent something within `timeout`, else local. Per LED, or global?
6. **Velocity → LED level palette** for note/CC-driven LEDs (mono, bicolour, later RGB): adopt MCU's 0/1/127 (off/blink/on), Novation-style palettes, or our own?
7. **Unacknowledged host→device SysEx**: allow fire-and-forget commands (needed for streaming feedback), which breaks the "every request gets a reply" rule in `protocol/sysex.md`?
8. **Manufacturer SysEx ID**: MIDI-CI Discovery (and a product release) need a real ID instead of `0x7D`. Is getting one planned?
9. **Where the final design note goes**: the issue asks for `protocol/host-feedback.md`; this research note is in `research/protocols/`. Write the protocol note once 1–7 are decided?
10. **YAML naming**: `source:` / `sync_from_host:` / `takeover:` / `mcu:` are placeholders. ESPHome has no direct equivalent; ok to invent, or model it on ESPHome automations (`on_value`)?

## 7. Not verified

- Whether Logic, Cubase, REAPER or Bitwig require the MCU challenge/response handshake (Live does not).
- Current DAW MIDI-CI/PE support (only a single secondary source).
- Status of Apple's MIDI-CI classes on macOS 15+.
- Whether Bitwig's generic mappings send value feedback; status of Live's UserConfiguration.txt in Live 12.
- Live's MCU script reading keyboard notes on other channels as MCU buttons: inferred from the decompiled script, not tested.
- All flash/RAM figures are estimates.
