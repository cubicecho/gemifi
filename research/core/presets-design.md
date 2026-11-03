# Presets and device modes: design (draft for review)

Written 2026-11-02. Status: phase 1 (presets) and phase 2 (modes) are implemented; phases 3 and 4 are not. Phase 2's deviations from this design are listed at the end of §6.

## Goal

A **preset** is a named set of *behaviours*: what the controls do, not how they are wired. The device YAML defines the presets. The protocol exposes them, and the user can switch them at runtime, either from the keyboard itself (e.g. Edit, then a key, then a numbered key) or from the web editor.

Both targets need this:

- **Keystation Mini 32.** It has no preset button, so switching needs a key-based "edit mode".
- **Code 61.** The stock firmware already has 12 presets and a preset button, and it has a display.

## 1. What goes in a preset

Every param gets a **scope**: *preset* (each preset has its own value) or *global* (one value for the device). The firmware fixes the scope for each param; the YAML does not choose it. **Decided 2026-11-02.**

**The rule: hardware side is global, mapping is per preset.**

- **Hardware side (global): turning a physical reading into a clean internal number.**
  - A pot's raw `adc_min`/`adc_max`.
  - An encoder's steps per detent.
  - The keybed's velocity calibration (`velocity.fast`/`slow`: which contact times count as hardest and softest on this keybed).
- **Mapping (per preset): what that internal number becomes on the way out.**
  - Which CC or note is sent, and on which channel.
  - The output range and curve.
  - The encoder mode.
  - How fast glides and ramps move.

The scope may become configurable later; for now it is fixed.

Velocity today: contact time → (calibration: fast/slow → 1..127) → (curve) → MIDI velocity. The middle step is global and the curve is per preset.

| Param | Proposed scope | Why |
|---|---|---|
| `button.action`, `button.number` | preset | the main reason presets exist |
| `analog.cc`, `encoder.cc`, `encoder.mode` | preset | |
| `midi.channel` | preset | e.g. a drums preset on channel 10 |
| `velocity.curve`, `velocity.fixed` | preset | playing feel per sound |
| `midi.transpose`, `midi.octave` | preset (start value) | the octave buttons change the live value; a switch reloads the preset's start value (§9 Q2) |
| `velocity.fast`, `velocity.slow` | global | they calibrate the keybed, so they are a property of the hardware |
| `adc_min`/`adc_max`, `steps_per_detent` (YAML-only today) | global | hardware calibration; if they ever become params, they stay global |
| `bend.glide`, `ramp.time` | preset | |
| `debug.*` | global | |
| new: `preset.active` | global | the preset to load at power-up (the last one used) |

The pins, the hubs, the matrix shape, the LED wiring and the USB IDs never go in a preset: they stay in the device YAML, as now.

## 2. Device YAML

```yaml
presets:
  slots: 8                      # how many presets the device keeps (YAML ones + empty ones)
  list:
    - name: Default             # preset 1 = the component defaults written in the YAML above
    - name: Drums
      midi: { channel: 10 }
      velocity: { curve: hard }
      buttons:                  # by component id; only what differs from the defaults
        sustain: { action: note, note: 36 }
        edit:    { action: note, note: 38 }
      analog:
        volume: { cc: 11 }
    - name: Ableton
      buttons:
        sustain: { action: cc_momentary, cc: 64 }
```

- **Preset 1 is the YAML defaults**, so a device without `presets:` behaves exactly as today, with one preset.
- **Other presets list only differences** from preset 1, by component id and with the same keys as the component itself. The builder checks them like the components (unknown id, a `cc:` on an action that has none, values in range…).
- **Empty slots** (`slots` minus the listed presets) start as copies of preset 1, named "Preset N".
- **The builder output** is a constant table per preset: a sparse list of (param id, index, value) overrides. It lives in flash and costs no RAM.

## 3. Firmware model

- **RAM.** `gm_cfg` holds the active preset plus the globals, as today. Only one preset is ever in RAM, which matters on the 4 KB NUC120.
- **Loading preset n.** Apply the param defaults, then preset n's YAML overrides, then preset n's stored record (§4). Global params are not touched.
- **Switching** (`preset_select(n)`):
  1. Release everything held: send Note Off for held keys and note buttons, release CC-momentary buttons, stop bend and ramps.
  2. Load preset n, then reset the toggles and the pick-up state.
  3. Update the LEDs and send PRESET_CHANGED to the host (§5).
  4. Send the preset's Program Change, if it has one (`program:` in the YAML, param `preset.program`, 0 = none).
  5. Store `preset.active`, but not on every switch (§4, wear).
- **Unsaved edits.** A host SET_PARAM changes the live values only. Switching preset without COMMIT discards those edits, like a hardware synth, and the editor gets a `dirty` flag so it can warn first. **Decided 2026-11-02.**

## 4. Storage

Today, the NUC keeps a journal of full-profile records in 7 × 512 B data-flash pages, and the newest record wins. A full Keystation record is about 260 B, so 8 full presets (~2 KB) would leave the journal too little room to rotate.

Proposal:

- **Record format v2.** Every record carries a `slot` (0xFF = globals, 1..N = preset). A preset record holds only the values that differ from that preset's YAML values, so an untouched preset costs nothing and a typical edit costs a few words.
- **Loading.** The newest valid record per slot wins.
- **Compaction.** Before the journal erases a page that holds the newest record of some slot, it copies that record forward. This keeps today's guarantee: the newest copy is never erased before a newer one exists. It needs one spare page, as now.
- **`preset.active`** is written only when it has been stable for about 10 s, or at COMMIT, so flipping through presets doesn't wear the flash.
- **Migration.** An old record (magic `GCFG`, no slot) is read as "globals + preset 1", and the next COMMIT writes v2. FACTORY_RESET erases everything, so the YAML presets return.
- **Budget.**
  - Keystation: 3.5 KB journal; 8 presets with typical edits plus globals is well under one page each.
  - **Code 61: gemifi takes over the stock preset pages. Decided 2026-11-02.**
    - **Pages:** the 12 stock preset pages (0x0801BC00–0x0801EBFF) and the names page (0x0801F800), plus the spare pages 0x0801EC00–0x0801F7FF. That is about 19 KB for the journal.
    - **Never touched:** the calibration page (0x0801B400), the global settings page (0x0801B800) and the update-flag page (0x0801FC00).
  - **Going back to stock firmware** then needs care, because the stock app would read gemifi records as its presets. Plan:
    1. **Before the first write.** The first time gemifi runs, it doesn't touch those pages until the host has saved a backup. `gemifi.py`/the web page reads the stock preset pages over a new read-only command and saves them as a file. gemifi refuses COMMIT until the backup is confirmed or skipped on purpose.
    2. **`restore-stock`.** While gemifi is still running, it writes the backup back (or erases the pages), then hands over to the vendor bootloader.
    3. **To check in the disassembly:** what the stock app does with blank (FF) preset pages. If it falls back to its built-in defaults, erasing is a safe restore even without a backup.
  - **The `0x67` caveat** (hw-map.md §3: the stock editor command can write preset slots 13/14) only matters while stock firmware runs, so it doesn't apply to gemifi.

## 5. Protocol (sysex.md additions)

Existing editors keep working: **GET/SET_PARAM always address the active preset**, and globals as before.

- **Schema flag.** Bit 5 = `PRESET` marks a per-preset param. It adds no bytes, so `schema_version` stays 1, and old editors ignore it.
- **New params:**
  - `preset.active`: global, 1..slots, changing it switches preset;
  - `preset.count`: read-only;
  - `preset.dirty`: volatile, read-only.
- **New commands** (proposed ids 0x0B–0x0E, all app-only):

| cmd | Name | Request | Reply |
|---|---|---|---|
| 0x0B | PRESET_INFO | `slot` u14 (0 = all, one reply per slot) | `slot`, `flags` (1 = YAML preset, 2 = has stored edits), `name` (string ≤ 16) |
| 0x0C | PRESET_SAVE | `slot` u14 | – : COMMIT the live values into `slot` (save-as) and switch to it |
| 0x0D | PRESET_RESET | `slot` u14 | – : drop the slot's stored edits (back to its YAML values) |
| 0x0E | PRESET_NAME | `slot` u14, `name` | – : rename (stored like an edit) |

- **COMMIT.** Saves the globals plus the live values into the *active* slot.
- **Unsolicited messages.** New 0x63 PRESET_CHANGED (`slot`, `dirty`) is sent when the user switches on the device. EVENT gets a type for mode changes (§6) so the web page can show "Edit mode".
- **Backups.** The host can read all presets by switching to each one, reading its params, then switching back; the tool does this. A read-another-slot command could come later if switching proves annoying.
- **HELLO caps.** Bit 5 = presets.

## 5a. Firmware updates and stored presets

**Today.** The stored profile survives a firmware update: values are tagged by param id, so params a new firmware adds get their defaults, and ones it dropped are ignored.

**With presets, the same rule holds.** A stored preset is the *changes* the user made on top of that preset's YAML values. After an update, those changes are applied on top of the *new* firmware's YAML presets. So:

- if the new YAML changes a value the user never touched, the new value is used;
- if the user changed it, their value wins.

**When the YAML presets (and later the modes) change a lot,** the user may want the new firmware's presets as they are. So uploads get a choice, made by the host and needing nothing new in the firmware upload path:

| Upload option | What the host does after the upload | Default |
|---|---|---|
| Keep my presets | nothing (changes apply on top of the new YAML presets) | yes |
| Use the firmware's presets | PRESET_RESET on every slot (drops the stored changes; names too) | no |
| Back up first | reads every preset (and later the modes) to a file before the upload, whichever option is chosen | yes in the web updater |

- **Spotting changed presets.** The YAML gets an optional `presets: { revision: 3 }`, and every stored record remembers the revision it was saved under. After an upload, the new firmware reports both in PRESET_INFO: "built with revision 3, your edits were made on revision 2". When they differ, the updater asks: "the presets in this firmware changed; keep your edits on top, or use the new presets?" It never resets without asking. The backup taken before the upload makes either choice undoable. The `.gfw` header doesn't need to change.
- **Modes later.** Once modes are editable, they follow the same rule: stored mode edits are changes on top of the YAML modes, and the same upload choice covers them.

## 6. Switching on the device: modes

The Keystation has no spare buttons, so switching needs a **mode**: a temporary layer that changes what the keys (and buttons) do. Edit → E → the white keys is one mode leading to another. Modes are device UI, so they are **global and defined in the YAML, not part of presets** (otherwise a preset could lock you out of switching back).

```yaml
modes:
  - id: edit
    toggle: { button: edit }         # Edit enters the mode, Edit again leaves it (from any sub-mode)
    led: edit_led                    # lit while edit mode (or a sub-mode) is active
    keys:                            # note names at octave 0 / no transpose (the physical key)
      E3: { mode: preset_pick }      # go to a sub-mode
      C3: { action: octave_reset }
      F3: { action: preset_prev }
      G3: { action: preset_next }
  - id: preset_pick
    parent: edit                     # after an action here, return to edit mode
    led: { edit_led: blink }
    keys:
      white_from: D4                 # 8 consecutive white keys = preset 1..8
      action: preset_select
```

**Exit. Decided 2026-11-02:** a mode stays active until its toggle button is pressed again; there is no timeout and no automatic exit after an action. A sub-mode (`parent:`) returns to its parent after one action, so Edit → E → D picks preset 1 and leaves you in edit mode, ready for another action, until Edit is pressed. A per-mode `exit:` option (after_action / timeout) can come later if a device needs it.

- **While a mode is active,** key presses run the mode's bindings and send no notes. Keys already held when the mode starts still get their Note Off, and unbound keys are silent. The buttons keep their normal actions unless the mode rebinds them (`buttons: { oct_up: { action: preset_next } }`).
- **New actions,** usable on buttons and in modes: `preset_next`, `preset_prev`, `preset_select` (+ number), `octave_reset`, `mode` (+ mode id, for a button that enters a mode directly). On the Code 61, the preset button simply gets `preset_next`, or enters a pick mode.
- **Feedback.** The mode LED plus, on the Keystation, a short blink pattern of the octave LEDs showing the preset number after a switch: Oct − blinks once per preset number, then both octave LEDs flash once to finish. On the Code 61, later, the 6-character display.
- **Generated code.** The builder turns modes into constant tables (mode → key → action/argument). The firmware cost is one small state machine plus the tables, and about 4 bytes of RAM.

### 6.1 Phase 2 as implemented (2026-11-02)

Implemented as above (core/README.md "Modes"), on the Keystation with Edit as the toggle: Edit → E3 → a white key from D4 picks preset 1..8, and the edit mode also has octave, transpose, channel and velocity-curve keys. Not tested on hardware yet. Where this section was vague or the implementation differs:

- **No `mode` button action.** A root mode is entered only by its `toggle:` button, so `button.action` keeps its 16 values and the schema, host and web lists don't change. A button that enters a mode directly can come later if a device needs one.
- **The toggle button has no action of its own,** and the builder refuses a preset that rebinds it (that is what would lock you out). A `button.action` the host sets for it is stored but ignored.
- **No `buttons:` rebinding inside a mode** yet: buttons keep their normal actions in every mode. *Added 2026-11-03 (§6.2).*
- **No EVENT type for mode changes** (§5): the protocol is unchanged. The toggle button's presses still show as button EVENTs.
- **Held keys** get their Note Off when the mode starts, not at their release.
- **Keys:** named by note at octave 0 (C4 = 60, so the Keystation's lowest key is C3) or by MIDI note number. Besides the design's `white_from:` range there is `from:` (every key) and `count:`; a range can be `preset_select` or `channel`. An action with no number can be written as just its name (`C3: octave_reset`).
- **Key actions:** `preset_select`, `preset_next`, `preset_prev`, `octave_down/up/reset`, `transpose_down/up/reset`, `channel`, `velocity_curve`, and `{ mode: <id> }`. `channel` and `velocity_curve` are device-side edits of per-preset params: they send PARAM_CHANGED and make the preset dirty, like a host edit.
- **LEDs:** a root mode lights its toggle button's LED by default, and a sub-mode blinks its parent's. The preset-number blink (Q7) is the per-mode option `preset_blink: [count LED, done LED]`, inherited by sub-modes; it runs after a preset switch made from a mode (not from a button or the host): one dark step, the slot number in 200 ms blinks, then both LEDs flash for 400 ms.
- **Keystation YAML:** Edit lost its CC toggle 80 (it is the mode button now); the Drums snare moved from Edit to Mod, and the DAW preset's Edit CC 81 was dropped.
- **Cost on the Keystation:** +844 B flash (19884 → 20728 B text) and +12 B RAM, tables included.

### 6.2 Hold modes and button bindings (2026-11-03)

For the Code 61: Up / Down step through the presets, and with Channel held they change the MIDI channel.

- **`hold: { button: <id> }`** is a third way into a root mode, beside `toggle:` and `parent:`. The mode is active only while the button is held. It can't have sub-modes, and it goes first when a toggled mode is also active. If the button is held through a preset switch, its release still ends the mode.
- **`buttons: { <button id>: <action> }`**, in any mode: the key actions (not sub-modes or ranges) for buttons. A bound button pressed in the mode runs that instead of its own action, and its release is silent even after the mode ends.
- **`channel_up` / `channel_down`** (GM_MK 13, 14): new actions for keys and buttons; 16 wraps to 1.
- **A mode with no `keys:` leaves the keyboard playing** (a Channel-held mode shouldn't silence it).
- **The display** shows the preset's name on any switch (scrolling it on the Code 61's 6 characters), and `CH <n>` on a device-side channel change (core/README.md "Displays").
- **Storage:** the STM32F1's single 1 KB page holds 3 Code 61 slots, at 96% in the worst case (the builder warns). More slots need the store to span more flash pages.

## 7. Tools and web

- **Builder.** Validates `presets:` and `modes:` (ids exist, keys are within the keyboard's range, no key bound twice in a mode, every mode reachable, every mode has an exit). Generates the preset override tables and the mode tables. The dashboard shows the presets list.
- **`gemifi.py`.** `presets` (list), `preset select|save|reset|rename`, and backup/restore covering all slots.
- **Web editor.**
  - A preset selector above the param form, with the active preset, a dirty marker and save / save-as / reset / rename.
  - Per-preset params are marked in the form, and globals sit in their own section.
  - The page follows PRESET_CHANGED when the user switches on the keyboard.

## 8. Phases

1. **Core presets.**
   - Scope flags, the record v2 journal with compaction, YAML `presets:`, `preset_next/prev/select` button actions, the protocol commands and the `gemifi.py` commands.
   - Tests: core host tests, the host-sim journal (power cut during compaction), and the builder.
   - On hardware: the Keystation with the presets on buttons.
2. **Modes.** YAML `modes:`, the key-layer state machine, the Keystation Edit → E → white-keys flow and the LED feedback. Hardware-test on the Keystation. **Implemented (§6.1); not yet tested on hardware.**
3. **Web editor:** the preset UI.
4. **Code 61,** after the STM32 platform lands: the preset button, the display, and optionally importing the 12 stock presets (decoding the vendor preset format from the disassembly).

Each phase is usable on its own: 1 already gives presets switchable from buttons and from the web.

## 9. Questions to settle

Decided 2026-11-02:
- **Q1, scope:** fixed in the firmware. The hardware side is global and the mapping is per preset (§1).
- **Q3, unsaved edits:** discarded on switch; the editor warns.
- **Q5, mode exit:** stay until Edit is pressed again (§6).
- **Q8, Code 61 storage:** take over the stock preset pages, with a backup and restore path (§4).

Also decided 2026-11-02:
- **Q2, octave/transpose:** a switch resets them to the new preset's starting values.
- **Q4, modes:** YAML only for now; they become editable over SysEx later (§5a covers what that means for firmware updates).
- **Q6, Program Change:** an option per preset: `program: 5` in the YAML (and a per-preset param `preset.program`, 0 = don't send).
- **Q7, Keystation feedback:** an octave-LED blink pattern for the preset number is enough.
- **Q9, slots:** set by the user in the YAML (`presets: { slots: N }`, default = the number of listed presets). The builder works out the worst-case storage for the platform, errors if it can't fit and warns when it's tight (§10.5). The suggested values are 8 on the Keystation and 12 on the Code 61.

No open questions are left for phase 1.

## 10. Phase 1 interfaces (for the implementation)

These pin down the seams between the builder, the core, the platform stores and the host tools so they can be built in parallel. The headers are already in the tree: `core/include/config_store.h`, and the preset types at the end of `core/include/gm_device.h`.

### 10.1 Generated code (builder → core)

`gm_device_config.h` gains:

```c
#define GM_N_PRESETS 8            /* presets.slots; 1 when the YAML has no presets: */
#define GM_PRESETS_REVISION 0     /* presets.revision, 0..0x3FFF; default 0 */
```

`gm_device_config.c` gains:
- `const gm_preset_def_t gm_presets[GM_N_PRESETS]`;
- one `static const gm_preset_val_t` array per preset that has overrides.

An entry's `(id, index)` is a param id from `gm_settings.h` and the component's index in its list. Slot 1 has no overrides. Each other preset lists only the values that differ from slot 1, sorted by (id, index). Empty slots have the name "Preset N", no overrides and no program.

### 10.2 YAML

```yaml
presets:
  slots: 8                 # optional; default = len(list); 1..64
  revision: 1              # optional; 0..16383
  list:
    - name: Default        # <= 16 chars, printable ASCII; optional (default "Preset N")
      program: 0           # optional 0..127: Program Change sent when selected
    - name: Drums
      midi: { channel: 10, transpose: 0, octave: 0 }
      velocity: { curve: hard, fixed: 100 }
      bend: { glide: ... }   ramp: { time: ... }    # the per-preset globals, same units as their params
      buttons:  { <button id>: { action: note, note: 36 } }      # same keys as a button entry
      analog:   { <analog id>: { cc: 11 } }
      encoders: { <encoder id>: { cc: 21, mode: relative_offset } }
```

The first list entry is slot 1. Its only allowed keys are `name` and `program`, because its values are the components' own defaults.

### 10.3 Core

- **Param scope.** `PF_PRESET` (bit 5) marks a per-preset param, using the §1 table, set in `settings.c`. `params.c` passes the flag through to the schema.
- **New params:**
  - id 17 `preset.active`: global, int 1..GM_N_PRESETS. A set switches preset.
  - id 18 `preset.program`: PF_PRESET, int −1..127, −1 = off.
  - id 19 `preset.dirty`: VOLATILE, READ_ONLY, bool.

  They go in a new "Presets" group, only when GM_N_PRESETS > 1, except `preset.active`, which is always present.
- **New button actions:** 13 `preset_next`, 14 `preset_prev`, 15 `preset_select` (`button.number` = slot). Mackie Control moves from 13 to **16**: its value changes, so a stored MCU profile from before needs re-setting. That's acceptable because MCU hasn't been tested on hardware yet.
- **New `core/src/presets.c` (+ `gm_presets.h`).** At boot:
  1. load slot 0, then `preset.active`;
  2. load the active slot: defaults → YAML overrides → stored record entries;
  3. select = release held notes and buttons, load, reset octave/transpose to the preset's values, send the Program Change, then PARAM_CHANGED and PRESET_CHANGED;
  4. a host SET_PARAM on a PF_PRESET param sets `dirty`.
- **COMMIT** writes slot 0 (the globals, including `preset.active`) and the active slot's diff (the values that differ from its YAML values).
- **FACTORY_RESET** = `config_store_erase()`, then reload slot 1.
- **`preset.active` wear.** A switch writes it only after it has been stable for 10 s (`gm_poll`), or at COMMIT.

### 10.4 Protocol (sysex.md)

App-only. "string" = `u8 len` (0..16) + 7-bit ASCII. HELLO caps bit 5 = presets.

| cmd | Name | Request | Reply |
|---|---|---|---|
| 0x0B | PRESET_INFO | `slot` u14 (0 = the active one) | `slot` u14, `count` u14, `active` u14, `flags` (bit 0 has stored edits, bit 1 dirty, bit 2 YAML-defined), `revision` u14 (firmware), `stored_revision` u14 (0 if no stored edits), `program` s32, `name` string |
| 0x0C | PRESET_SAVE | `slot` u14 | – : store the live per-preset values (as a diff vs slot's YAML values) and the globals, make `slot` active, clear dirty |
| 0x0D | PRESET_RESET | `slot` u14 (0 = all) | – : drop stored edits and name; reloads if active |
| 0x0E | PRESET_NAME | `slot` u14, `name` string | – : store a new name (empty = back to the YAML name) |
| 0x63 | PRESET_CHANGED (unsolicited) | `slot` u14, `dirty` (byte) | sent when the active preset changes or dirty flips |

Select = SET_PARAM `preset.active`. A bad slot → BAD_PARAM; a name too long or not ASCII → BAD_VALUE.

### 10.5 Storage budget (builder check)

- **Worst-case record for a preset:** 4 header words + 4 name words + 2 words × (number of PF_PRESET params × their counts) + 1 CRC word.
- **Worst-case record for the globals:** the same with the global params.
- **Rule 1:** both must fit one record on the platform: `config_store_max_entries()`, and one page on the NUC.
- **Rule 2:** all live records at worst case must fit, with room left for rewriting.
  - NUC journal: 1 + slots records, packed whole per page, in **pages − 2**. One page is the erase spare, and one is room for a slot's old and new copy while it is rewritten; the power-cut tests showed pages − 1 isn't enough.
  - ESP8266: the sum of all records ≤ 4096 B.

| Platform | Store | Budget |
|---|---|---|
| nuvoton-nuc1xx | data-flash journal | 7 pages × 512 B, 2 kept free (5 usable); a named record ≤ 59 entries (508 B) |
| esp32 | NVS, one blob per slot | 12 KB total (default NVS partition is 20 KB; leave room); a blob ≤ 1984 B (243 entries) |
| esp8266 | EEPROM sector, one image | 4096 B for all slots together (a power cut during a write can lose the whole sector, as before presets) |
| stm32f1 | two 1 KB pages that swap (Code 61: 0x0801EC00, 0x0801F000) | 1024 B for every slot together, in packed v3 records (platform/stm32f1/src/config_store.c): one group per param, int8/int16 values and an index bitmap. Code 61: a preset ≤ 276 B, so 3 slots + the globals |

- **Error** when worst case doesn't fit.
- **Warning** when worst case uses more than 85% (more erase cycles). The message gives the numbers and the largest `slots` that fits.
- **Also reported** in `gemifi_build.py` output and the dashboard: "presets: 8 slots, worst case 2.9 of 3.0 KB".
