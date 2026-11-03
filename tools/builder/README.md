# gemifi builder

ESPHome-style builder for gemifi. You describe the hardware in a **device YAML** (or start from a preset), and the builder validates it, generates the C tables the core is compiled with (`gm_device_config.h` / `.c`), and runs the platform build to produce a firmware image.

The YAML format, the hardware model (virtual pins, hubs, components, button actions) and the generated-code contract are defined in [`core/README.md`](../../core/README.md). This file only covers the tool.

## Setup

Python 3.8+ and **PyYAML**. Check with `python3 -c "import yaml"`. If that fails, install it with `pip install pyyaml` or `sudo apt install python3-yaml`, or use a venv:

```sh
python3 -m venv tools/builder/.venv && tools/builder/.venv/bin/pip install pyyaml
tools/builder/.venv/bin/python tools/builder/gemifi_build.py ...
```

Platform toolchains (only needed for `build`):
- NUC1xx: `arm-none-eabi-gcc` + `make` (see `platform/nuvoton-nuc1xx/`).
- STM32F1: the same (see `platform/stm32f1/`).
- ESP32 / ESP8266: PlatformIO (`pio` on `PATH`, or `~/.venvs/pio/bin/pio`).

## Commands

```sh
B=tools/builder/gemifi_build.py

$B validate devices/keystation-mini32-mk3.yaml       # check it, print errors as file:line
$B validate devices/*.yaml                           # several at once (exit 1 if any fails)
$B generate devices/keystation-mini32-mk3.yaml -o /tmp/cfg   # write gm_device_config.h/.c only
$B build    devices/keystation-mini32-mk3.yaml       # validate + generate + firmware build
$B presets                                           # list presets in devices/
$B new my-keys --preset keystation-mini32-mk3        # copy it to ~/gemifi-devices/my-keys.yaml
$B dashboard                                         # web UI on http://127.0.0.1:6055/ (flags below)
```

**validate** reports errors (which stop a build) and warnings (strapping/flash pins, ADC2, I2C-polled encoders and so on), each with `file:line` and the YAML path:

```
mine.yaml:14: error: buttons[2].pin: 'PE9' is not a pin on nuvoton-nuc1xx (valid: PA0-PA15, PB0-PB15, PC0-PC15, PD0-PD15)
mine.yaml:31: error: analog[0].pin: PB0 has no ADC on nuvoton-nuc1xx (ADC pins: PA0-PA7; or use a mux hub channel)
mine.yaml:40: error: key_matrix.keys: 40 keys don't fit a 8 x 8 velocity matrix (at most 32)
```

It checks: the format version (`gemifi: 2`; a format 1 file gets one error per old key saying what replaced it), the node `name` (lowercase, digits, `-`, at most 31), the platform and its pin names (NUC1xx `PA0`…`PD15`; ESP `GPIOn` / `n` per chip, `A0` on ESP8266), pin modes against what the pin is used for and what the chip can do (no pull-downs on NUC1xx, no pulls on ESP32 GPIO34–39, no `open_drain` yet), pins used twice (component pins, hub control pins and I2C pins all count), ADC-capable pins for analog inputs (or a mux whose `common_pin` is an ADC pin), output-capable pins for LEDs and matrix rows, hub ranges (`sr_count` 1–4, 8/16 mux channels, 16 MCP23017 / 8 PCF8574 pins) and that a hub pin names its hub with the right chip key (`{ pcf8574: p, number: 0 }`), which hubs each use allows (outputs: 595/MCP/PCF; inputs: 165/mux/MCP/PCF/switch_matrix; matrix: MCU, 595 or decoder rows, 165 columns; switch_matrix: MCU or decoder rows, MCU columns; encoders: not on a mux), decoder groups (the same enable pins in each, distinct levels), ids (below), `led:`, `boot_combo` and hub references, button `cc:`/`note:`/`preset:` against the action, the `presets:` section (below), time periods (`125us`, `1.6ms`: a bare number is an error that says which unit to add), the key matrix (velocity needs even columns, or even rows with `contacts: rows`; keys fit the matrix; a `key_map` has one entry per position and each key once; `first_note + keys ≤ 128`), pads (an ADC pin or an analog mux channel each, `note` 0..127, a warning for two pads on one note; `pad_settings` ranges with `release < threshold < full`), `board_id ≤ 0x3FFF`, at most 64 buttons, I2C pins when an I2C hub exists, action names, 0..127 numbers, and unknown keys (with "did you mean").

**Ids** follow ESPHome's rules: a C identifier (a letter, then letters, digits and `_`), not a C/C++ keyword, unique across the whole device (hubs and components share one namespace). gemifi also wants them lower_snake_case. Ids that YAML would read as something other than a name are refused with the reason: PyYAML follows YAML 1.1, so a bare `no`, `off`, `On`, `yes` or `true` is a boolean, `null` is nothing and `12` / `0x1F` are numbers; `y`, `n` and quoted `'no'` are refused too, because other YAML tools read them differently. A component with only a `name:` gets an id derived from it (`Pitch Down` → `pitch_down`); two derived ids that collide are an error naming both entries.

**Presets** (`presets:`, the switchable sets of behaviours of [`core/README.md`](../../core/README.md) "Presets"; not the `devices/` templates below). The builder checks them like the components: `slots` 1..64 and at least the number listed, `revision` 0..16383, names (≤ 16 printable ASCII, unique, also against the "Preset N" of empty slots), `program` 0..127, preset 1 taking only `name`/`program`, component ids (with "did you mean"; an id of the wrong kind says what it is), the keys an override may have (hardware keys like `pin:`, `adc_min:`, `led:` and globals like `velocity.fast` or `midi.transport` are refused with where to set them instead), `cc:`/`note:`/`preset:` against the effective action (an override of an action that takes no number is an error here, not a warning), ranges and units, enum names (`curve`, encoder `mode`) and params the device doesn't have (`velocity.curve` without a velocity matrix, `bend`/`ramp` without buttons, `pad_settings` without pads; its sensing keys are refused as global). An override equal to preset 1's value is dropped with a warning. The preset actions (`preset_next`, `preset_prev`, `preset_select`) need 2 or more slots.

**Storage budget.** The builder works out the worst case of what the device stores (research/core/presets-design.md §10.5): a record per preset holding every per-preset value plus a name, and one for the globals. The per-preset / global split of the params lives in one table, `PARAMS` in `gemifi_device.py`, mirroring `core/src/settings.c`. Each record must fit the platform's record limit, and all of them (1 + `slots`) the platform's store:

| Platform | Store | Budget |
|---|---|---|
| nuvoton-nuc1xx | data-flash journal | 6 usable 512 B pages (7, one kept spare); a record ≤ 512 B and never spans a page |
| esp32 / esp32s2 / esp32s3 | NVS, a blob per slot | 12 KB in all, a blob ≤ 1984 B |
| esp8266 | EEPROM sector | 4096 B for every slot together |
| stm32f1 | two 1 KB config pages, swapped | 1024 B for every slot together. Records are packed (v3, one group per param: int8 or int16 values with a bitmap of the stored indices, `packed_record_bytes()`; `WIDE_PARAMS` lists the int16 ones). Code 61: a preset ≤ 276 B, the globals ≤ 100 B, so up to 3 slots (2 under 85%) |

Not fitting is an error that says the largest `slots` that fits; using more than 85% is a warning (a fuller store means more erase cycles) that also says how many slots stay under 85%. Without `presets:`, a record that doesn't fit is only a warning (it was so with the old record format). The summary line shows the numbers:

```
devices/keystation-mini32-mk3.yaml: OK: Keystation Mini 32 (...), presets: 8 slots, worst case 1836 B in 5 of 6 pages (3072 B)
```

Two hubs can share a control line (e.g. an sn74hc595 and an sn74hc165 on one clock and latch, or two muxes on the same select lines) only if each use sets `allow_other_uses: true` (`clock_pin: { number: GPIO18, allow_other_uses: true }`) and the line has the same job in each (clock with clock, latch with load). Component pins can't be shared. Making it work electrically is up to you.

**build** writes into `build/devices/<name>/` (`<name>` = the YAML file name without `.yaml`; `-o` picks another directory):

| Path | What |
|---|---|
| `config/gm_device_config.{h,c}` | generated tables |
| `fw/` (NUC) or `pio/` (ESP) | the platform's build directory |
| `<image>.gfw`, `<image>.bin` | the image(s), copied here at the end. `<image>` is the node name and version, as ESPHome names its builds: `keystation-mini32-0.1.5` (just the node name without `project: { version }`). The previous build's images are removed first. |

- NUC1xx: `make -C firmware/nuvoton-nuc1xx/midi-controller DEVICE_DIR=<config> BUILD=<out>/fw`, image `midi-controller.gfw`.
- STM32F1: `make -C firmware/stm32f1/midi-controller DEVICE_DIR=<config> BUILD=<out>/fw`, image `midi-controller.gfw` (installed through the keyboard's own bootloader; `tools/README.md`).
- ESP: `GM_DEVICE_DIR=<config> GM_PIO_EXTRA=<out>/board.ini PLATFORMIO_BUILD_DIR=<out>/pio pio run -d firmware/esp/midi-controller -e <platform>`. `board.ini` sets `board = <chip>` for that env (pio has no command-line board override). Outputs copied to `<out>/`: `<image>.bin` (the app, for OTA over SysEx), `<image>-factory.bin` (merged, flash at 0x0 for a first install), `<image>-bootloader.bin`, `<image>-partitions.bin`, and `webflash/` (ESP Web Tools `manifest.json` + parts).

Flash the image with the **web updater** (`cd web/updater && npm run dev`) or `tools/host/gemifi.py`. The builder doesn't flash.

## Presets

A preset is any `devices/*.yaml`. `presets` lists them with their `platform` and top-level `description:`. `new <name> --preset <preset>` copies one to `~/gemifi-devices/<name>.yaml` (`--dir` picks another directory, `--force` overwrites) for you to edit; when `<name>` is a valid node name (lowercase, digits, `-`) it also becomes the copy's `name:`. To add a preset, put a valid YAML with a `description:` line in `devices/`. `tests/test_builder.py` checks that every preset validates and compiles.

## Dashboard

`gemifi_build.py dashboard` is the ESPHome-dashboard equivalent: a web server that lists the presets (`devices/`) and your devices (the config dir, default `~/gemifi-devices/`), and lets you create a device from a preset, edit its YAML, validate it (errors inline with line numbers), build it (the log streams in) and download the images. It serves the **web updater** at `/` when it is built (`cd web/updater && npm ci && npm run build`, or `--web`), so the same page flashes the image over Web MIDI / Web Serial. Without a build it serves a small inline page. It uses the standard library only (plus PyYAML) and runs one build at a time.

| Flag | Env var | Default |
|---|---|---|
| `--host H` | `GEMIFI_HOST` | `127.0.0.1` |
| `--port N` | `GEMIFI_PORT` | `6055` (not ESPHome's 6052, so both can run on one box) |
| `--web DIR` | `GEMIFI_WEB` | `web/updater/dist` if it has an `index.html`, else the inline page |
| `--config-dir DIR` (`--user-dir`) | `GEMIFI_CONFIG_DIR` | `~/gemifi-devices` (the `mine/` root) |
| `--build-dir DIR` | `GEMIFI_BUILD_DIR` | `build/devices`; outputs go to `<dir>/<presets\|mine>/<name>/` |
| `--allowed-host H` (repeatable) | `GEMIFI_ALLOWED_HOSTS` (comma-separated) | none; `name` matches any port, `name:port` only that port |
| `--tls-cert F --tls-key F` | `GEMIFI_TLS_CERT`, `GEMIFI_TLS_KEY` | plain http |
| | `GEMIFI_PASSWORD`, `GEMIFI_USERNAME` | no auth; with a password every request needs HTTP Basic auth (user `gemifi` by default) |
| | `GEMIFI_READONLY_PRESETS=1` | presets are read-only anyway when `devices/` isn't writable |

Security:
- **Host header** (DNS rebinding). On a loopback bind only `127.0.0.1`, `localhost` and `[::1]` on the server's port are accepted, plus any `--allowed-host`. On another bind, the allowed hosts plus the loopback names on any port; with no allowed hosts **any** Host is accepted and a warning is printed (set a password).
- **POSTs must be `application/json`**: a cross-site form can't send that without a CORS preflight, which the server never answers.
- **Read-only presets**: with `GEMIFI_READONLY_PRESETS=1` (the container) or a non-writable `devices/`, saving or deleting a preset is a 403 and the listing says `writable: false`. Copy one with New (`/api/new`).
- Device ids are `presets/<file>.yaml` or `mine/<file>.yaml`; anything else (`..`, subdirectories, other extensions) is a 400. Static files can't escape the web dir.
- **Web MIDI and Web Serial need a secure context**: `https://`, or `http://localhost`. From another machine use TLS (`--tls-cert/--tls-key`, or the container's self-signed mode) or an SSH tunnel (`ssh -L 6055:localhost:6055 buildbox`, then open `http://localhost:6055/`).

### HTTP API

Everything is under `/api/`. Errors are `{"error": "<message>"}` with a 4xx status (400 bad id or body, 401 auth, 403 Host / read-only, 404, 409 busy, 413, 415 not JSON). POST bodies are JSON objects.

| Request | Response |
|---|---|
| `GET /api/info` | `{"name": "gemifi builder", "version": "0.1.0", "user_dir", "toolchains": {"nuvoton-nuc1xx": bool, "stm32f1": bool, "esp": bool}}`: NUC and STM32F1 = `arm-none-eabi-gcc` and `make` found, ESP = `pio` found |
| `GET /api/devices` | `{"devices": [{"id", "root": "presets"\|"mine", "file", "name", "platform", "description", "images": [str], "writable": bool}], "user_dir"}`; `images` = the `.gfw`/`.bin` files at the top of that device's output dir |
| `GET /api/presets` | `{"presets": [{"name", "platform", "description"}]}` |
| `GET /api/file?id=` | `{"id", "text"}` |
| `POST /api/validate {"id"?, "text"}` | `{"ok", "summary", "diags": [{"level": "error"\|"warning", "path", "line", "msg"}], "presets": {"slots": [{"slot", "name", "program", "changes", "listed"}], "revision", "given", "storage": {...the budget numbers}} \| null}` |
| `POST /api/save {"id", "text"}` | `{"ok": true}`; 403 on a read-only preset |
| `POST /api/new {"name", "preset"}` | `{"id"}` (`mine/<name>.yaml`); 400 if it exists or the name is bad |
| `POST /api/delete {"id"}` | `{"ok": true}`; `mine/` only (403 for presets), also removes its build outputs; 409 during a build |
| `POST /api/build {"id"}` | `text/plain`, the build log streamed line by line, last line `[exit N]` (0 = OK); 409 JSON if a build is running |
| `GET /api/image?id=&file=` | `application/octet-stream` with `Content-Disposition: attachment`, one of the names in `images` (`/download` is an alias) |

Any other path is a static file from the web dir (`/` = `index.html`; `assets/*` cached for a year as immutable, the rest `no-cache`; extensionless paths fall back to `index.html`).

## Docker

The repo root has a `Dockerfile` and `docker-compose.yml`: the dashboard plus the built web updater, `arm-none-eabi-gcc` + `make` for NUC builds, and PlatformIO for ESP builds.

```sh
docker compose up -d --build            # http://localhost:6055/
# or
docker build -t gemifi .
docker run -d --name gemifi -p 6055:6055 -v ./config:/config -v gemifi-cache:/cache gemifi
```

- **`/config`** (`./config`): your device YAMLs (the `mine/` root), builds in `/config/.build/`, the self-signed cert in `/config/.tls/`. The presets are baked into the image and read-only (`GEMIFI_READONLY_PRESETS=1`); New copies one into `/config`.
- **`/cache`** (named volume): `PLATFORMIO_CORE_DIR=/cache/platformio`. The image has no ESP toolchains: **the first ESP build downloads about 6 GB** into it (later builds reuse it). NUC builds need nothing extra. To reuse a host's toolchains, mount `~/.platformio` at `/cache/platformio`.
- **User**: the entrypoint starts as root, fixes the ownership of the cache and build dirs, and runs the server as `PUID:PGID` (default `1000:1000`; `PUID=0` stays root). Or set `user: "1000:1000"` in compose and make sure both volumes are writable by that user.
- **Password**: set `GEMIFI_PASSWORD` (and optionally `GEMIFI_USERNAME`) whenever the port is reachable from other machines; anyone who can reach it can edit and build. Set `GEMIFI_ALLOWED_HOSTS` to the names you use to reach it.
- **TLS**: a browser on another machine needs `https://` for Web MIDI / Web Serial. `GEMIFI_TLS=self-signed` makes a certificate once (P-256, valid 825 days) for `GEMIFI_TLS_HOSTS` (comma-separated names and IPs, default `localhost`; a `:port` is allowed and ignored, IPv6 as `[::1]` or `::1`; `localhost` and `127.0.0.1` are always added) and serves https; accept it in the browser once. Plain `http://` on the same port is redirected to `https://`. Changing `GEMIFI_TLS_HOSTS` makes a new one. Or give your own with `GEMIFI_TLS_CERT` / `GEMIFI_TLS_KEY` (paths inside the container, e.g. under `/config`). The alternative is plain http and an SSH tunnel to the box, so the browser sees `localhost`.
- The image is about 1 GB (192 MB compressed). `.dockerignore` keeps `research/` (copyrighted stock firmware), build outputs, `node_modules` and `./config` out of the build context.

### Published image (GHCR)

GitHub Actions (`.github/workflows/docker.yml`) builds the image and pushes it to `ghcr.io/cubicecho/gemifi`, for `linux/amd64` and `linux/arm64`.

| Tag | When |
|---|---|
| `latest`, `1.2.3`, `1.2` | a `v1.2.3` git tag |
| `edge` | every push to `main` |
| `<branch>` | a manual run (Actions → docker → Run workflow) on that branch; `/` becomes `-` |
| `sha-<short>` | every published build |

Pull requests build the image (amd64 only) but don't push it.

The first push creates the package as **private**. Either:
- make it public: the package page on GitHub → Package settings → Change visibility → Public. The org must allow public packages.
- or log in on the docker host: `docker login ghcr.io -u <github-user>`, with a personal access token (classic) that has `read:packages` as the password.

### Deploy on a docker host

The host doesn't need the source tree. `docker-compose.ghcr.yml` runs the published image.

```sh
mkdir -p ~/gemifi && cd ~/gemifi
# copy docker-compose.ghcr.yml from the repo to here as docker-compose.yml, e.g.
#   scp docker-compose.ghcr.yml dockerhost:gemifi/docker-compose.yml
cat > .env <<'EOF'
GEMIFI_TAG=latest
GEMIFI_TLS_HOSTS=dockerhost.lan,192.168.1.20
GEMIFI_ALLOWED_HOSTS=dockerhost.lan,192.168.1.20
GEMIFI_PASSWORD=change-me
EOF
docker compose pull
docker compose up -d
docker compose logs -f        # "gemifi dashboard: https://..."
```

- `GEMIFI_TAG`: `latest` exists only after the first `v*` release. Before that, use `edge` or a branch tag.
- `GEMIFI_TLS_HOSTS`: the names and IPs you type in the browser. The cert must match them.
- `./config` must be owned by `PUID:PGID` (default `1000:1000`).
- Update: `docker compose pull && docker compose up -d`. `./config` and the cache volume are kept.

`docker-compose.yml` is for working on the source: it builds the image from the repo with `--build`. The two files are kept apart so a deploy never tries to build, and a local build never gets the GHCR name.

### Flash from the browser

**Web MIDI only works in a secure context**: `https://`, or `http://localhost`. Over plain `http://dockerhost:6055/` the page loads, but it sees no MIDI devices.

1. Plug the device into the machine with the browser (not the docker host).
2. Open `https://dockerhost.lan:6055/` in Chrome or Edge.
3. The cert is self-signed, so the browser warns. Choose Advanced → Proceed. Do it once per cert.
4. Log in with `gemifi` and `GEMIFI_PASSWORD`.
5. Build the device, then flash it. When the browser asks for MIDI device control (SysEx), allow it. If you blocked it, reset it under the padlock → Site settings → MIDI devices.

No TLS? Use an SSH tunnel so the browser sees `localhost`: `ssh -L 6055:localhost:6055 dockerhost`, then open `http://localhost:6055/`.

## Tests

```sh
cd tools/builder && python3 -m unittest
```

What the tests check:
- the Keystation YAML generates tables with the board's shape (a small program, `tests/dump_device.c`, compiled with host gcc against `core/include`, prints every field). Plain `make` in `firmware/nuvoton-nuc1xx/midi-controller` and `core/test` generate from that YAML too, so `core/test` checks the generated tables' behaviour on every run.
- `generate` leaves unchanged files alone (same mtime), so `make` doesn't rebuild when the YAML hasn't changed.
- every generated `.c` (the Keystation, every preset, `tests/fixtures/everything-esp32s3.yaml` with all hub types and components, and an empty device) compiles with `gcc -std=c11 -Wall -Wextra -Werror`. The `core/src/*.c` files are compiled against each one, and `settings.c` + `params.c` are linked against it.
- validation errors (file, line, path, message), the CLI, and the dashboard: every API endpoint, per-id output dirs, read-only presets, static serving and traversal, Basic auth, the Host-header rules, the `GEMIFI_*` env vars, and (with `arm-none-eabi-gcc`) a real NUC build through `/api/build`.
