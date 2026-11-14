# Coding standards ledger

Findings from the `coding-standards` skill's refactor workflow. This file lives at
`.agents/coding-standards-todos.md`. IDs are stable — don't renumber when items are removed.
`(unverified)` marks items inferred from docs or naming rather than confirmed in code.
Nothing here is implemented until approved.

Baseline (origin/main 2181c2d, 2026-11-14): builder 228 passed; core 23 test programs, 0 failed;
cs-test, boot-sim, host-sim, host tools OK; schema up to date; all 10 `devices/*.yaml` generate and
build. Generated configs and images saved in `~/.cache/gemifi-components-baseline/`.

## Summary

| Prefix | Kind |
|---|---|
| `M` | Model change: a change to `.agents/domain-model.md`, made true by the items that implement it |
| `R` | Refactor: same behavior, better shape |
| `F` | Feature: new behavior |
| `T` | Test work |
| `D` | Comment or doc that no longer matches the code |
| `A` | Public API change; needs a decision |

| ID | Kind | What it does | Buys | Needs | Status |
|---|---|---|---|---|---|
| M1 | Model change | A component is one folder holding its C, its builder part, its docs and its tests; a device builds only the components it uses. | Adding or reading a component means one folder, not four files | — | approved |
| M2 | Model change | Base components define an interface that platform components implement; the hub is the base for chips that give pins. | One place says what a hub (or a screen) must do | — | approved |
| M3 | Model change | I2C becomes a list of buses with ids; I2C components name theirs with `i2c_id`. | Two I2C buses on an ESP32; ESPHome-shaped YAML | — | approved |
| M4 | Model change | Screens are a `display` base component (`display: - platform: ssd1306`), not hubs. | The OLED stops being a hub without pins; HD44780 or SPI OLEDs fit later | — | approved |
| M5 | Model change | Displays (the lines of text) keep their name and meaning; a text display names its screen with `display:`. | Displays work the same on any screen | — | approved |
| M6 | Model change | The core keeps only what isn't a component: protocol, params, settings, controller loop, presets, MIDI, safety, MCU. | A smaller core whose files each own one thing | — | approved |
| M7 | Model change | The controls (buttons, LEDs, analog inputs, encoders, key matrix, pads, modes, rings) become components split out of the controller. | `controller.c` (3.9k lines) stops mixing ten concepts | — | approved |
| R1 | Refactor [pattern] | The builder finds components by folder, and the hub base class and its derived lists move into the `hub` component. | M1, M2; the builder's output is unchanged | — | done |
| T1 | Test [pin] | Builder tests for component discovery and for the generated source lists. | Pins R1 and R2 | — | done |
| R2 | Refactor [pattern] | The builder writes the list of component sources a device uses, and every build (make, ESP-IDF, host tests) compiles from it. | M1: only used components are compiled, no glob of the core | R1 | done |
| R3 | Refactor [pattern] | The hub interface moves into the `hub` component, and each hub type's own state moves out of the shared union into its component. | M2: a new hub type stops editing shared core files | R2 | done |
| R4 | Refactor [move] | The WS2812 driver, builder class and docs move into `components/ws2812/`, as the template for the rest (changed from the SSD1306, which A1 moves once, as a display). | M1; the shape every other hub follows | R3 | done |
| R5 | Refactor [sweep] | Every other hub type except the SSD1306 (A1) moves into its own component folder the same way, one commit each (types that share a driver share a folder). | M1, M2; `vpin.c` keeps only pin dispatch | R4 | approved |
| R6 | Refactor [pattern] | The controls move out of the controller into one component each, after a short design note on the state they share, keeping the scan's hot paths as fast. | M7 | R2 | deferred: after the display work is merged (2026-11-14) |
| F1 | Feature | Several I2C buses with ids, `i2c_id` on I2C components, the HAL taking a bus; the current `i2c: { sda, scl }` still means one bus. | M3 | R5 (the I2C hubs) | approved |
| A1 | API change | The `display` base and `platform: ssd1306` under it; text displays name their screen with `display:`; the old `ssd1306:` forms are refused with the new form in the message. Breaking for YAMLs with an OLED. | M4, M5 | R4, F1 | done |
| D1 | Docs | `core/README.md`'s general hub sections become "Components" and "Adding a component", pointing at each component's README. | The docs match M1 | R5 (R6's sections later) | approved |

## Model changes

### M1 — split the hub into one folder per component

As-is: a hub type is in four places (`vpin.c` driver, `gm_hub.h` state, `gm_device.h` config,
`gemifi_device.py` class) plus a section of `core/README.md`; every core file is compiled and unused
drivers are gated out with `GM_HUB_TYPES`. To-be: `components/<name>/` holds `<name>.c/.h`,
`__init__.py` (schema, validation, generated C), `README.md` and tests; the builder discovers
components and lists the sources a device uses. Why it is simpler: one folder per concept. Implemented
by: R1, R2, R3, R4, R5. Status: approved.

### M2 — base components and platforms

As-is: one hub concept and one ops table for pin chips, LED strips and a screen. To-be: a base
component defines an interface (`hub`, `display`, `i2c`); platform components implement it.
Pattern: interface (ops table) + per-component state. Implemented by: R1, R3, A1. Status: approved.

### M3 — I2C buses with ids

As-is: `i2c: { sda, scl }`, one bus, `hal_i2c_*(addr, ...)`. To-be: `i2c:` a list of `{ id, sda, scl,
frequency }` (a single mapping still allowed), `i2c_id:` on I2C components (optional with one bus),
`hal_i2c_*(bus, addr, ...)`. Implemented by: F1. Status: approved.

### M4 — screens are a display base, not hubs

As-is: `ssd1306:` is a hub with no pins whose framebuffer is in hub RAM, drawn by `vpin_oled_draw`.
To-be: `display: - { id, platform: ssd1306, i2c_id, address, ... }`; a screen ops interface (init, draw
glyph columns at column / row / scale, send within a budget) with a shared mono framebuffer part.
Old forms refused with the fix in the message. Implemented by: A1. Status: approved.

### M5 — displays name their screen

As-is: a text display says `ssd1306: oled`. To-be: `display: oled`. Segment displays unchanged.
Implemented by: A1. Status: approved.

### M6 — the core keeps what isn't a component

Implemented by: R5, R6 (what's left in `core/`). Status: approved.

### M7 — controls are components

As-is: buttons, LEDs, analog inputs, encoders, key matrix, pads, modes and rings are sections of
`controller.c` sharing file statics. To-be: a component each, with an internal header for the state
they share. Implemented by: R6. Status: approved.

### M8 — other base components (to propose after A1)

The user asked to look at the other components after the display base and give them the same shape
(e.g. LED strips, LED drivers, I/O expanders). Proposed after A1, at its own gate. Status: open.

---

## Conventions

- Delivery (decided 2026-11-14): one PR per stage: base (T1, R1-R4), hubs (R5), I2C buses (F1),
  display (A1, D1); R6 after those are merged.
- Generated files and the command that rebuilds them: `site/src/schema/device.json` →
  `python3 tools/builder/gemifi_build.py schema -o site/src/schema/device.json`.
- Every refactor commit is checked against the baseline: builder, core and platform tests, schema
  check, the 10 devices' generated configs (identical, except where an item says why not) and builds.
- Commits: conventional (`refactor:`, `feat:`, `docs:`, `test:`), one item each, merge (not rebase)
  to update from main.

## Refactoring

### R1 [pattern] — discover components; move the hub base class

**File:** `tools/builder/gemifi_device.py:332-891` (`HubType`, subclasses, `HUB_DEFS` and the sets
derived from it). A `components` package at the repo root; `components/hub/__init__.py` holds
`HubType`; the builder imports every `components/<name>/__init__.py` and builds `HUB_DEFS` from the
`HubType` instances they define. The subclasses stay where they are until R4/R5 move them. The
classes use `NO_PIN`, `_comment`, `rgb_curve`, `validate_rgb_calibration`, `validate_decoder`,
`validate_xy_pad`, `resolve_switch_matrices` from `gemifi_device.py`: shared helpers go to the `hub`
component, type-specific ones move with their type. Implements: M1, M2. Buys: generated configs
unchanged. Needs: T1.

### R2 [pattern] — generated source lists

**Files:** `firmware/*/midi-controller/Makefile`, `platform/esp/gemifi_esp/CMakeLists.txt`
(`file(GLOB core/src/*.c)`), `core/test/Makefile`. The builder writes `gm_components.mk` and
`gm_components.cmake` next to `gm_device_config.h`: the used components' `.c` files and include
dirs. Object names must stay unique (`platform/stm32f1/mk/rules.mk` uses `notdir`). Implements: M1.

### R3 [pattern] — the hub interface as a base component

**Files:** `core/src/gm_hub.h`, `core/src/vpin.c:2374-2460` (`hub_ops[]`). `gm_hub_ops_t`, the common
state and `ram_alloc` go to `components/hub/`; each type's own state struct moves into its component
and is reached through the hub's state (a per-type state array the builder sizes, or a member the
component declares) (unverified: RAM per hub may change by a few bytes; measured on the 10 builds).
`hub_ops[]` keeps its `USES()` gate, with `extern` ops from each component.

### R4 [move] — `components/ws2812/`

**Files:** `core/src/vpin.c` (WS2812 section, its ops, the weak `hal_ledstrip_*` defaults),
`gemifi_device.py` (`WS2812`, `WS2812_MAX`, `WS2812_ORDERS`), `core/include/gm_device.h`
(`gm_hub_ws2812_t`), `components/hub/gm_hub.h` (its state), `core/README.md` "WS2812 LED strips". The
template: folder layout, README shape, test placement. The hub base exports what the driver used from
`vpin.c` (the hub RAM, `ram_alloc`, the colour output shared with `maudio_pad_leds`), and the builder
helpers shared by full-colour hubs (`rgb_curve`, `validate_rgb_calibration`) move to `components/hub`.
Changed 2026-11-14 from the SSD1306: A1 turns it from a hub into a display platform and rewrites most
of its driver, so it moves once, there.

### R5 [sweep] — the other hub types as components

sn74hc595, sn74hc165, cd74hc4051/4067, mcp23017, pcf8574, decoder, switch_matrix, xy_pad, tm1628,
maudio_pad_leds, ws2812, max7219, tm1637, ht16k33. One commit each; types sharing a driver share a
folder, named after the ESPHome component where there is one. The hot paths written out in
`vpin.c` (mux and cached reads, decoder rows, the switch-matrix scan) stay in `vpin.c` or become
`static inline` in the component's header, so a pin costs what it does now.

### R6 [pattern] — the controls as components

**File:** `core/src/controller.c` (3.9k lines: key matrix 291, encoders 687 and 2361, hardware scan
819, velocity 1020, buttons 1143, modes 1388, analog 1852, pads 2145, LEDs 2483, rings 3026, host
feedback 3074). First a short design note (in the PR) on the state the sections share and the
internal header that holds it, for approval; then one component per control with its builder part.
Measured with the scan timing the tests and `performance.md` use.

---

## Features

### F1 — I2C buses with ids

Done when: two buses work on an ESP32 build, `i2c_id` resolves (optional with one bus), the old
mapping form is one bus, stm32f1 / NUC still refuse I2C components, schema and docs updated.

---

## Docs

### D1 — "Components" and "Adding a component"

**File:** `core/README.md:33-110` and the Files table. Rewritten for the component folders once R5 and
R6 have moved the type-specific sections out.

---

## API changes (need a decision)

### A1 — `display:` with `platform: ssd1306`

Breaking: YAMLs with `ssd1306:` (top level) or `ssd1306: <id>` in `displays:` are refused, with the
new form in the message. Migrates `devices/diy-esp32-pot-oled.yaml`, `devices/diy-esp32s3-pot-oled.yaml`,
the docs and the schema. Decided 2026-11-14: a `feat:` commit with no breaking marker (a minor bump); the release notes and the
builder's message explain the new form.
