<!-- SPDX-License-Identifier: MIT -->
# gemifi for Bitwig Studio (controller extension)

A Bitwig Studio controller extension that drives **any** gemifi device with host feedback (HELLO caps bit 6, `protocol/sysex.md` §4.6). It's the Bitwig counterpart of `hosts/ableton-live/`. It learns the device's controls with GET_COMPONENT, hears presses and moves through EVENT messages, and lights LEDs, sets pick-up targets and drives encoder rings with the FB_* notifications. Nothing in it is specific to one device: the component `role`s (`hosts/README.md`) say what each control does.

Written from scratch against Bitwig's public controller API (`com.bitwig.extension.*`). It contains no code from Bitwig's or anyone else's extensions. The protocol, device state machine and mapping are a Java port of this project's own Live script. MIT licensed.

**Status: untested in real Bitwig.** It compiles against `extension-api` 25 and its unit tests pass (see "Tests"), but it has never been loaded into Bitwig Studio or run against a real device. Treat everything under "What it does" as the intended behaviour, not as verified behaviour.

## Build

You need JDK 17 or newer and Maven 3.6 or newer.

```
cd hosts/bitwig
mvn package
```

This produces `target/gemifi.bwextension`. A `.bwextension` is the jar under another name. The Bitwig API is fetched from Bitwig's Maven repository (`https://maven.bitwig.com`) at `provided` scope. It isn't bundled: Bitwig Studio supplies it at run time.

`mvn test` runs only the unit tests.

## Install

1. Copy `target/gemifi.bwextension` into Bitwig's extensions folder:
   - macOS: `~/Documents/Bitwig Studio/Extensions/`
   - Windows: `%USERPROFILE%\Documents\Bitwig Studio\Extensions\`
   - Linux: `~/Bitwig Studio/Extensions/`

   The folder is in your user documents folder. It's created when Bitwig first starts.
2. In Bitwig: **Settings → Controllers → Add controller**. Pick hardware vendor **gemifi**, model **gemifi**.
3. Set its MIDI input and output to the device's **main** port: the one `tools/host/gemifi.py` talks to. Don't use its "DAW" (Mackie Control) port. The protocol is only spoken on the main port (`protocol/sysex.md` §7).

Auto-detection is off. Port names differ per device and per OS, and a device with a DAW port must not have that port picked, so you add the controller yourself. With two devices, add the controller twice.

When the extension finds the device, Bitwig shows a pop-up: `gemifi: connected to <node> (fw x.y.z)`. Diagnostics go to the controller script console (Settings → Controllers → the controller's ⋯ menu → Show console, or *Help → Show Control Script Console* in some versions), prefixed `gemifi:`.

The extension passes every channel message from the device (notes, CCs, bend, pressure, program change) to Bitwig's tracks through a note input called "gemifi". Keys and pads keep playing instruments, and CCs reach whatever they're mapped to. The extension maps none of them; it reads only SysEx.

## What it does

1. On `init()`, it sends HELLO. If nothing answers, it tries again about every 3 s. A device without caps bit 6 (older firmware, or the bootloader) gets no further messages.
2. It sends GET_COMPONENT for each kind (button, keys, analog, encoder, LED, display), from index 0 up to `count`. BAD_PARAM at index 0 means the device has none of that kind.
3. It reads `debug.events` (param 13), then sets it to 1.
4. Once ready, on every `flush()` (Bitwig calls it after observed values change) and on a 100 ms tick (`host.scheduleTask`), it compares the project's state with what it last sent and sends only the differences: FB_LED, FB_LEDS for runs of neighbouring LEDs, FB_VALUE and FB_RING. FB_VALUE goes only to analog inputs with `takeover` pick-up and to absolute encoders, and LEDs with `owner` local get nothing (GET_COMPONENT's added fields, §4.6.1; firmware without them gets everything, as before).
5. When the device sends BOOTED (§4.6.3: it restarted, so its FB_* states are gone), the extension starts again from HELLO and re-sends everything. As a fallback it reads `debug.events` back: about every 5 s from firmware without BOOTED, about every 30 s from firmware with it (BOOTED can be missed if the port opens late). That param is volatile (never stored), so a timeout or a 0 means the device was unplugged or restarted, and the extension starts again from HELLO.
6. On `exit()` (the controller is removed or disabled, or Bitwig quits), it sends FB_RELEASE 0 and sets `debug.events` back to what it was.

Requests follow §1's stop-and-wait rules:
- One request is outstanding at a time.
- A resend keeps the same `seq`.
- Up to 6 sends in total (on a timeout, BAD_CHECKSUM or BUSY).
- The timeout is 3 ticks (200–300 ms).

Notifications go out at any time, and only to a device with caps bit 6.

## Roles

`mcu:` names are mapped first, as `hosts/README.md` says:
- `fader_N` → `volume_N`, `vpot_N` → `pan_N`, `rec_N` → `arm_N`
- `master` → `master_volume`
- `bank_left/right` → `bank_prev/next`, `channel_left/right` → `strip_prev/next`
- `cycle` → `loop`, `click` → `metronome`

Unknown roles are logged and ignored. `x_…` roles and other hosts' commands (`zynthian.…`) are ignored without a log line. A role Bitwig can't do is logged as "not supported in this host".

A role on a button acts on press, and the button's LED (if it has one) shows the state. A role on a kind-5 LED shows the same state. LED levels are 127 on, 0 off and 1 blink.

| Role | Button press (Bitwig API) | LED |
|---|---|---|
| `play` | `Transport.play()` / `stop()` (toggle) | lit while playing |
| `stop` | `Transport.stop()` | lit while held |
| `record` | toggle `isArrangerRecordEnabled` | lit while recording; **blinks** while armed but stopped |
| `loop` | toggle `isArrangerLoopEnabled` | lit while on |
| `metronome` | toggle `isMetronomeEnabled` | lit while on |
| `overdub` | toggle `isArrangerOverdubEnabled` | lit while on |
| `session_record` | toggle `isClipLauncherOverdubEnabled` (launcher overdub, Bitwig's nearest match) | lit while on |
| `automation_arm` | toggle `isArrangerAutomationWriteEnabled` | lit while on |
| `tap_tempo` | `Transport.tapTempo()` | lit while held |
| `undo`, `redo` | `Application.undo()` / `redo()` | lit while held |
| `rewind`, `fast_forward` | `Transport.incPosition(∓4 beats)`, repeated every tick after ~300 ms held | lit while held |
| `track_prev`, `track_next` | `CursorTrack.selectPrevious()` / `selectNext()` | lit if there is one |
| `track_mute`, `track_solo`, `track_arm` | toggle on the cursor (selected) track | that track's state |
| `mute_N`, `solo_N`, `arm_N` | toggle on strip N of the 8-track bank | that track's state |
| `select_N` | select strip N's track (`CursorTrack.selectChannel`) | lit when it is the selected track |
| `bank_prev`, `bank_next` | scroll the bank by 8 tracks | lit if it can scroll |
| `strip_prev`, `strip_next` | scroll the bank by 1 track | lit if it can scroll |
| `device_prev`, `device_next` | `CursorDevice.selectPrevious()` / `selectNext()` | lit if there is one |
| `device_bank_prev`, `device_bank_next` | previous / next remote-controls page | lit if there is one |
| `device_on` | toggle the selected device's `isEnabled` | lit while on |
| `scene_launch` | launch the scene under the 1-scene scene bank | lit while held |
| `scene_prev`, `scene_next` | scroll that scene bank by one | lit if it can scroll |
| `stop_all_clips` | `trackBank.sceneBank().stop()` | lit while held |

Continuous roles work on analog inputs (absolute, 0..16383) and encoders (signed deltas):

| Role | Target |
|---|---|
| `track_volume`, `track_pan` | the cursor track's volume / pan |
| `send_a`, `send_b` | the cursor track's sends 1 and 2 |
| `volume_N`, `pan_N` | strip N of the bank (`createMainTrackBank(8, …)`: audio, instrument and group tracks; no effect tracks or master) |
| `master_volume` | the master track's volume |
| `crossfader` | `Transport.crossfade()` |
| `tempo` | `Transport.tempo()`: 1 BPM per encoder detent (`incRaw`); an analog input spans Bitwig's tempo range |
| `device_param_N` | remote control N of the selected device's current page (`CursorRemoteControlsPage` of 8) |
| `cue_volume` | **not supported**: Bitwig's API has no cue / pre-listen volume. Logged and ignored |

- **Analog inputs** use pick-up. After the target changes in Bitwig (mouse, automation, a bank switch), the knob does nothing until it comes within 3 % of the value or crosses it. The extension also sends the value as FB_VALUE to an input with `takeover: pickup`, so the device does the same for its own MIDI; an input with `takeover: none` gets no FB_VALUE (the extension's own pick-up still applies). Values are set as `value / 16383` normalised. After a knob move, the extension ignores the parameter's reported value for ~300 ms, because Bitwig applies `set()` asynchronously and would otherwise look like a change from elsewhere.
- **Encoders** move a parameter with `inc(delta, 128)`: 1/128 of its range per detent. On Bitwig 5.2+ (API 20), a quantized parameter (`discreteValueCount()` > 1) moves one value per detent. Encoders in absolute mode get FB_VALUE; relative ones (`mode` 1, 2, 3 or any later value) don't. Either way, an encoder with a ring gets FB_RING:
  - **Boost/cut** with the centre LED for bipolar parameters: pan, the crossfader, and on API 20+ any remote control whose origin (`getOrigin()`) is in the middle.
  - **Wrap** otherwise.
  - **Ring off** (flags bit 1) when the encoder has no target, e.g. no device or an empty remote-control slot.
- **Layers** (`protocol/daw-scripts.md` §5.11). A twin (GET_COMPONENT `layer` > 0) is live only while its mode is on, and EVENT 8 says when; the device replays the layers that are on when `debug.events` becomes 1. The device layer keeps that state (`Device.layersOn()`), but the mapping doesn't need it: every component has its own binding, so twins with one role in two modes (`device_param_1` in two encoder modes) both drive it and both get its feedback, and only live ones send EVENTs. `device_bank_prev` / `device_bank_next` keep stepping Bitwig's remote-control pages of 8, even on a layer with only `device_param_1..4`. Firmware without the `layer` byte: everything is live.
- Keys (kind 2), pads (kind 7) and displays (kind 6) are left alone.

## Files

| File | |
|---|---|
| `pom.xml` | Maven build; `extension-api` as `provided`, JUnit 5 and Gson (tests only) |
| `src/main/java/gemifi/bitwig/GemifiExtensionDefinition.java` | name / vendor / model "gemifi", one MIDI in/out pair, required API 18 |
| `src/main/java/gemifi/bitwig/GemifiExtension.java` | `init()` / `flush()` / `exit()`, SysEx in/out, the 100 ms tick |
| `src/main/java/gemifi/bitwig/BitwigModel.java` | the abstract model on Bitwig's Transport, CursorTrack, TrackBank, remote controls, master and scene bank |
| `src/main/java/gemifi/bitwig/core/Protocol.java` | SysEx codec: frames, 7-bit encodings, packing, lstring, HELLO, GET_COMPONENT, EVENT, BOOTED, FB_* |
| `src/main/java/gemifi/bitwig/core/Device.java` | device state machine: HELLO, enumeration, `debug.events`, retries, BOOTED, keep-alive |
| `src/main/java/gemifi/bitwig/core/Roles.java` | `mcu:` → role mapping |
| `src/main/java/gemifi/bitwig/core/Model.java` | the DAW as the mapping sees it (interfaces) |
| `src/main/java/gemifi/bitwig/core/Mapping.java` | roles → actions and change-only feedback, pick-up |
| `src/main/resources/META-INF/services/com.bitwig.extension.ExtensionDefinition` | how Bitwig finds the definition |

Nothing in `core/` imports Bitwig; a test checks that.

## Tests

```
cd hosts/bitwig && mvn test
```

You don't need Bitwig to run them. 75 tests:
- **Protocol:** every frame in `protocol/test-vectors.json` (HELLO, EVENT, the `feedback` section's GET_COMPONENT requests and replies, with and without the added fields, BOOTED and all FB_* frames), the `layer` byte and EVENT 8 frames from `daw-scripts.md`, the integer encodings, packing, the checksum and the bad frames.
- **Device:** a fake device answering per §4.6, covering a lost reply, BUSY, silence and rescan, the bootloader, a restart caught by BOOTED or by the keep-alive, the two keep-alive intervals, PARAM_CHANGED turning events off, the EVENT 8 replay (kept even before ready) and release.
- **Mapping:** a fake DAW covering every role group, pick-up (including a host that applies values late), rings, ring off, LED runs, the mcu: names, the added fields (no FB_VALUE to relative encoders or non-pick-up inputs, nothing to `owner: local` LEDs), twins sharing a role and the re-send after BOOTED.
- **Packaging:** the service file loads the definition, and `core/` stays free of Bitwig imports.

## Bitwig API notes (and how sure)

Sure enough. Each of these is in the API 25 javadoc, at API version 18 or lower:
- `ControllerExtensionDefinition` / `ControllerExtension`.
- `MidiIn.setSysexCallback` (it delivers a hex string), `MidiOut.sendSysex(String hex)` and `MidiIn.createNoteInput`.
- `host.scheduleTask(Runnable, ms)`, `createTransport`, `createApplication`, `createCursorTrack(id, name, sends, scenes, follow)`, `createMainTrackBank`, `createMasterTrack` and `createSceneBank`.
- `markInterested()` on every value read.
- `ObjectProxy.createEqualsValue` for `select_N`.
- `Scrollable.scrollBy` / `canScrollBackwards` / `canScrollForwards`.
- `CursorRemoteControlsPage.selectNextPage(false)`.

Less sure:
- **`trackBank.sceneBank().stop()` as "stop all clips".** The javadoc only says "stops clip launcher playback for the associated track". Check that it stops every track, not only the tracks in the 8-track window.
- **Scene selection.** Bitwig has no "selected scene" in the API, so `scene_prev/next` scroll a 1-scene `SceneBank` (with `setIndication(true)`) and `scene_launch` launches it. Whether the indication shows in the launcher has not been checked.
- **Whether `value().get()` updates synchronously after `set()`.** The ~300 ms settle window assumes it doesn't; if it does, the window does no harm.
- **`exit()` sending SysEx.** FB_RELEASE and the `debug.events` restore go out from `exit()`. If Bitwig has already closed the port by then, the device keeps the host states until it restarts or the next HELLO.
- **The comment line in the `META-INF/services` file.** `java.util.ServiceLoader` allows `#` comments (a test confirms it), but if Bitwig reads the file with its own parser and the extension doesn't appear, delete the SPDX line there first.
- **The Java level.** The API jar targets Java 8, and Bitwig's own extensions repo compiles for Java 21. This builds for Java 17 (`maven.compiler.release`). That's fine for Bitwig 5.x, which bundles Java 21, but would fail on a Bitwig that ships an older JVM.
- **`getRequiredAPIVersion()` 18.** It's meant to mean "loads in Bitwig 5.0 and later". Every call it makes is marked `@since` 18 or lower, except `getOrigin()` / `discreteValueCount()` (20), which are only called when `getHostApiVersion() >= 20`. The mapping from Bitwig version to API version has not been checked.
- **Arm on tracks that can't be armed** (groups, master). The extension doesn't check; it relies on Bitwig ignoring the set.

## Licence of the Bitwig API

The extension compiles against `com.bitwig:extension-api:25`, from Bitwig's public Maven repository `https://maven.bitwig.com`. That is the newest version there; its metadata lists only 25.
- **No licence file.** The jar and its sources jar have no licence file and no licence headers. The Bitwig website calls it an "open controller extension API".
- **Bitwig's own extensions** (`github.com/bitwig/bitwig-extensions`) are MIT licensed (© Bitwig GmbH) and consume the same artifact.
- **How it's used here.** It is a `provided` dependency: it's compiled against, never copied into this repository or bundled into the `.bwextension`. Bitwig Studio supplies it at run time.
- If you need certainty for redistribution, ask Bitwig. Nothing here redistributes their code.

## Open questions

- None of this has been tried in Bitwig: the install path, the controller list entry, SysEx in and out, the `flush()` timing, the pop-up.
- Spec points in `protocol/sysex.md` §4.6 the extension had to guess at, the same ones as the Live script:
  - A button's `led` is taken to be an index in the kind-5 (LED) list.
  - Encoder deltas are taken to be in detents without acceleration.
  - An encoder's `mode` can change while the extension runs (SET_PARAM `encoder.mode`, a preset change); the extension reads it once, at connect.
  - A device restart drops the FB_* states; the extension hears BOOTED, or (older firmware, or a BOOTED sent before the port was open) notices by polling `debug.events`.
