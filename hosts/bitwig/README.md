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
2. It sends GET_COMPONENT for each kind (button, keys, analog, encoder, LED, display, pad), from index 0 up to `count`. BAD_PARAM at index 0 means the device has none of that kind (firmware from before pads answers BAD_PARAM for kind 7).
3. It reads `debug.events` (param 13), then sets it to 1.
4. Once ready, on every `flush()` (Bitwig calls it after observed values change) and on a 100 ms tick (`host.scheduleTask`), it compares the project's state with what it last sent and sends only the differences: FB_LED, FB_LEDS for runs of neighbouring LEDs, FB_LED_RGB for clip LEDs, FB_VALUE and FB_RING. FB_VALUE goes only to analog inputs with `takeover` pick-up and to absolute encoders, and LEDs with `owner` local get nothing (GET_COMPONENT's added fields, §4.6.1; firmware without them gets everything, as before).
5. When the device sends BOOTED (§4.6.3: it restarted, so its FB_* states are gone), the extension starts again from HELLO and re-sends everything. As a fallback it reads `debug.events` back: about every 5 s from firmware without BOOTED, about every 30 s from firmware with it (BOOTED can be missed if the port opens late). That param is volatile (never stored), so a timeout or a 0 means the device was unplugged or restarted, and the extension starts again from HELLO.
6. On `exit()` (the controller is removed or disabled, or Bitwig quits), it sends FB_RELEASE 0 and sets `debug.events` back to what it was.

Requests follow §1's stop-and-wait rules:
- One request is outstanding at a time.
- A resend keeps the same `seq`.
- Up to 6 sends in total (on a timeout, BAD_CHECKSUM or BUSY).
- The timeout is 3 ticks (200–300 ms).

Notifications go out at any time, and only to a device with caps bit 6.

## Update policy

- **Change-only.** The extension keeps what it last sent for each LED (level or clip colour), value and ring, and sends only differences. When nothing changes, nothing goes out.
- **When it looks.** On every `flush()` and every 100 ms tick, and right after a button or pad EVENT.
- **Clip LEDs** go out as one FB_LED_RGB each (there is no batched RGB message), only for the LEDs whose colour or blink changed. A grid scroll sends only the clip LEDs that look different afterwards.
- **FB_VALUE is rate-limited in `flush()`.** Bitwig can call `flush()` many times a tick while values move (automation, a mouse drag). There, LEDs and rings go out at once, but each control gets at most one FB_VALUE per tick. A value held back goes out on the next tick, so the device always ends on the latest one. The pick-up reset happens at once either way.
- **Re-sending.** The caches are emptied on BOOTED, when the device is lost, and on exit, so the next refresh sends everything.
- **Encoders.** Bitwig applies `inc()` a little later, so the ring and FB_VALUE follow on the next `flush()` after the turn.
- **No echo.** A value an analog input just set is not sent back to it as FB_VALUE. Bitwig applies `set()` asynchronously, so the extension also ignores the host's value for ~300 ms after a knob move.
- **Cost on the Java side.** A refresh where nothing changed allocates nothing. Target keys are rebuilt only when the track, device or page changes, and incoming SysEx hex is parsed in one pass.

**What that costs on the wire.** Frames are 14 bytes for FB_LED, 14 + n for FB_LEDS, 16 for FB_VALUE and 17 for FB_RING and FB_LED_RGB.
- **Idle:** nothing, apart from a 14-byte keep-alive request every ~30 s.
- **Bank change:** up to 8 FB_VALUE for the faders, plus the strip LEDs that change, mostly as one FB_LEDS. On 8 pick-up faders and 8 LEDs that's 9 messages and about 150 bytes.
- **Clip grid:** a launch is one FB_LED_RGB when it's queued (blink) and one when it plays. A scroll of the Code 61's 4 × 4 grid is at most 16, 272 bytes.
- **Automation on N mapped parameters:** at most N FB_VALUE a tick, plus a ring per flush for an encoder with one. 8 pick-up faders under automation: about 1.3 KB/s, whatever the flush rate. Without the limit, at an assumed 30 flushes a second, that was about 3.8 KB/s, more than a DIN cable's ~3 KB/s.
- **At connect:** one GET_COMPONENT round trip per component, about 170 on a Code 61 (roughly 11 KB in both directions). Each request goes out from the SysEx callback as the previous reply arrives, so nothing waits on the tick.

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
| `clip_T_S` (T, S = 1..8) | the clip in column T, row S of the clip grid (below): launch it, or stop it (`Track.stop()`) while it plays or records. An empty slot: nothing | FB_LED_RGB, the clip's state (below) |
| `scene_N` (N = 1..8) | launch the scene in row N of the grid | lit while held |
| `grid_left`, `grid_right` | move the grid by one track | lit if it can move |
| `grid_up`, `grid_down` | move the grid by one scene | lit if it can move |

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
- **Pads** (kind 7, `protocol/daw-scripts.md` §5.9) work as buttons, with any button role. Base pads play notes and send no EVENT, so in practice only pad twins (a mode's `layer:`) do anything: EVENT 7 with value > 0 is the press, 0 the release. A missing `layer` byte means 0, and bytes after it are ignored.
- **The clip grid** (§5.9). A window of the session's clips, apart from the mixer bank: `clip_T_S` is the slot in its column T and row S, counted from its scroll position, which starts at track 1, scene 1 and is kept across a device restart. It's as wide and as tall as the largest T and S (and N of `scene_N`) the device has, so the Code 61's pad grid is 4 × 4. `grid_*` move it by one and stop where the window's last column (row) reaches the last track (scene); a project with fewer tracks than columns doesn't scroll. The clip roles also work on buttons, and on a kind-5 LED (the colour only). On the Code 61, Shift + Pad octave − turns the grid on; pads 1-16 are the clips and Pad octave −/+ are `grid_left`/`grid_right`.
- **Clip LED colours** (FB_LED_RGB):

  | Slot | Colour |
  |---|---|
  | empty | off |
  | a clip, stopped | the clip's colour (teal 0,127,127 if it has none) |
  | playing | green 0,127,0 |
  | recording, or about to | red 127,0,0 |
  | a launch, stop or record queued | that colour, blinking (flags bit 0) |

  So a queued launch blinks in the clip's colour, a queued stop blinks green. The Code 61 shows each channel on or off (lit if at least half the brightest), so a green clip looks like a playing one there, and greys look white. Single and bicolour LEDs get FB_LED_RGB too; the device lights them (§5.1). The twins' LEDs are set at any time; the device shows them while the layer is on.
- Keys (kind 2) and displays (kind 6) are left alone.

## Files

| File | |
|---|---|
| `pom.xml` | Maven build; `extension-api` as `provided`, JUnit 5 and Gson (tests only) |
| `src/main/java/gemifi/bitwig/GemifiExtensionDefinition.java` | name / vendor / model "gemifi", one MIDI in/out pair, required API 18 |
| `src/main/java/gemifi/bitwig/GemifiExtension.java` | `init()` / `flush()` / `exit()`, SysEx in/out, the 100 ms tick |
| `src/main/java/gemifi/bitwig/BitwigModel.java` | the abstract model on Bitwig's Transport, CursorTrack, TrackBank, remote controls, master, scene bank and the clip grid's track bank |
| `src/main/java/gemifi/bitwig/core/Protocol.java` | SysEx codec: frames, 7-bit encodings, packing, lstring, HELLO, GET_COMPONENT, EVENT, BOOTED, FB_* |
| `src/main/java/gemifi/bitwig/core/Device.java` | device state machine: HELLO, enumeration, `debug.events`, retries, BOOTED, keep-alive |
| `src/main/java/gemifi/bitwig/core/Roles.java` | `mcu:` → role mapping |
| `src/main/java/gemifi/bitwig/core/Model.java` | the DAW as the mapping sees it (interfaces) |
| `src/main/java/gemifi/bitwig/core/Mapping.java` | roles → actions and change-only feedback, pick-up, the clip grid |
| `src/main/resources/META-INF/services/com.bitwig.extension.ExtensionDefinition` | how Bitwig finds the definition |

Nothing in `core/` imports Bitwig; a test checks that.

## Tests

```
cd hosts/bitwig && mvn test
```

You don't need Bitwig to run them. 89 tests:
- **Protocol:** every frame in `protocol/test-vectors.json` (HELLO, EVENT, the `feedback` section's GET_COMPONENT requests and replies, with and without the added fields, BOOTED and all FB_* frames), the `layer` byte, EVENT 8, pad replies (with and without `layer`) and EVENT 7 frames from `daw-scripts.md`, the integer encodings, packing, the checksum, the bad frames and hex parsing.
- **Device:** a fake device answering per §4.6, covering a lost reply, BUSY, silence and rescan, the bootloader, a restart caught by BOOTED or by the keep-alive, the two keep-alive intervals, PARAM_CHANGED turning events off, the EVENT 8 replay (kept even before ready) and release.
- **Mapping:** a fake DAW covering every role group, pick-up (including a host that applies values late), rings, ring off, LED runs, the mcu: names, the added fields (no FB_VALUE to relative encoders or non-pick-up inputs, nothing to `owner: local` LEDs), twins sharing a role, the re-send after BOOTED, and one FB_VALUE per control per tick from `flush()`.
- **Clip grid:** pads enumerated with their layer, a twin's EVENT 7 launching and stopping a clip, a press on an empty slot, `scene_N`, scrolling and clamping (also after tracks are deleted), the LED colours and blink, change-only clip LEDs, the re-send after BOOTED (keeping the grid's position), clip roles on buttons and LEDs, and the `clip_T_S` names.
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

Fairly sure (every call is in the API 25 jar, checked with `javap`; their `@since` versions were not checked against the required API 18, and none of it has been tried):
- **The grid's own track bank.** `createMainTrackBank(8, 0, 8)`, made in `init()` at the largest size. Each track's `clipLauncherSlotBank()` slot has `hasContent`, `isPlaying`, `isRecording`, `isPlaybackQueued`, `isStopQueued`, `isRecordingQueued` and `color()`, all marked interested; the colour is read with `red()`/`green()`/`blue()`, so a refresh doesn't allocate. A press calls `ClipLauncherSlot.launch()`, or `Track.stop()` to stop (a slot has no stop of its own; only one clip per track plays). `scene_N` calls `Scene.launch()` on the bank's `sceneBank()`.
- **Scrolling** sets the bank's `scrollPosition()` and its `sceneBank().scrollPosition()`; the slots follow the scene bank. Clamping uses `itemCount()` of each.

Less sure:
- **Sizing the grid after `init()`.** At connect the extension calls `Bank.setSizeOfBank()` on the grid's track and scene banks (the roles' width and height, at most the capacity of 8). It's meant to make Bitwig scroll and indicate the window the device shows. If Bitwig refuses it outside `init()`, the error goes to the console and the bank stays 8 × 8; then Bitwig may keep the scroll position where an 8-wide window fits, so a 4-wide grid may stop scrolling 4 tracks early.
- **The grid indication.** `setIndication(true)` on the slot banks of the columns in use should draw the grid's rectangle in the launcher, possibly 8 scenes tall if the scene bank size doesn't reach the slot banks. Not checked.
- **What `itemCount()` counts** on a main track bank (flattened group children or not). The clamp uses it; Bitwig clamps the scroll position itself as well, so a wrong count only limits how far the grid moves.
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
  - Encoder deltas are steps, already accelerated by the device when it has `acceleration:` (`core/README.md` "Encoder acceleration"); the extension uses them as they come.
  - An encoder's `mode` can change while the extension runs (SET_PARAM `encoder.mode`, a preset change); the extension reads it once, at connect.
  - A device restart drops the FB_* states; the extension hears BOOTED, or (older firmware, or a BOOTED sent before the port was open) notices by polling `debug.events`.
