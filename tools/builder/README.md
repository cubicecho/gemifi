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
- ESP32 / ESP8266: PlatformIO (`pio` on `PATH`, or `~/.venvs/pio/bin/pio`).

## Commands

```sh
B=tools/builder/gemifi_build.py

$B validate devices/keystation-mini32-mk3.yaml       # check it, print errors as file:line
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

It checks: the platform and its pin names (NUC1xx `PA0`…`PD15`; ESP `GPIOn` / `n` per chip, `A0` on ESP8266), pins used twice (component pins, hub control pins and I2C pins all count), ADC-capable pins for analog inputs (or a mux whose `common` is an ADC pin), output-capable pins for LEDs and matrix rows, hub ids/types/ranges (`chain` 1–4, 8/16 mux channels, 16 MCP23017 / 8 PCF8574 pins), which hub types each use allows (outputs: 595/MCP/PCF; inputs: 165/mux/MCP/PCF; matrix: MCU, 595 rows, 165 cols; encoders: not on a mux), unique labels, `led:` and `boot_combo` references, the key matrix (velocity needs even cols, keys fit the matrix, `first_note + keys ≤ 128`), `board_id ≤ 0x3FFF`, at most 32 buttons, I2C pins when an I2C hub exists, action names, 0..127 numbers, and unknown keys (with "did you mean").

Two hubs can share a control line (e.g. a 74HC595 and a 74HC165 on the same clock and latch, or two muxes on the same select lines) only if every hub involved has `shared: true` and the line has the same role in each. Making that work electrically is up to you.

**build** writes into `build/devices/<name>/` (`<name>` = the YAML file name without `.yaml`; `-o` picks another directory):

| Path | What |
|---|---|
| `config/gm_device_config.{h,c}` | generated tables |
| `fw/` (NUC) or `pio/` (ESP) | the platform's build directory |
| `<name>.gfw`, `<name>.bin` | the image(s), copied here at the end |

- NUC1xx: `make -C firmware/nuvoton-nuc1xx/midi-controller DEVICE_DIR=<config> BUILD=<out>/fw`, image `midi-controller.gfw`.
- ESP: `GM_DEVICE_DIR=<config> GM_PIO_EXTRA=<out>/board.ini PLATFORMIO_BUILD_DIR=<out>/pio pio run -d firmware/esp/midi-controller -e <platform>`. `board.ini` sets `board = <chip>` for that env (pio has no command-line board override). Outputs copied to `<out>/`: `<name>.bin` (the app, for OTA over SysEx), `<name>-factory.bin` (merged, flash at 0x0 for a first install), `<name>-bootloader.bin`, `<name>-partitions.bin`, and `webflash/` (ESP Web Tools `manifest.json` + parts).

Flash the image with the **web updater** (`cd web/updater && npm run dev`) or `tools/host/gemifi.py`. The builder doesn't flash.

## Presets

A preset is any `devices/*.yaml`. `presets` lists them with their `platform` and top-level `description:`. `new <name> --preset <preset>` copies one to `~/gemifi-devices/<name>.yaml` (`--dir` picks another directory, `--force` overwrites) for you to edit. To add a preset, put a valid YAML with a `description:` line in `devices/`. `tests/test_builder.py` checks that every preset validates and compiles.

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
| `GET /api/info` | `{"name": "gemifi builder", "version": "0.1.0", "user_dir", "toolchains": {"nuvoton-nuc1xx": bool, "esp": bool}}`: NUC = `arm-none-eabi-gcc` and `make` found, ESP = `pio` found |
| `GET /api/devices` | `{"devices": [{"id", "root": "presets"\|"mine", "file", "name", "platform", "description", "images": [str], "writable": bool}], "user_dir"}`; `images` = the `.gfw`/`.bin` files at the top of that device's output dir |
| `GET /api/presets` | `{"presets": [{"name", "platform", "description"}]}` |
| `GET /api/file?id=` | `{"id", "text"}` |
| `POST /api/validate {"id"?, "text"}` | `{"ok", "summary", "diags": [{"level": "error"\|"warning", "path", "line", "msg"}]}` |
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
- **TLS**: a browser on another machine needs `https://` for Web MIDI / Web Serial. `GEMIFI_TLS=self-signed` makes a certificate once (P-256, valid 825 days) for `GEMIFI_TLS_HOSTS` (comma-separated names and IPs, default `localhost`; `localhost` and `127.0.0.1` are always added) and serves https; accept it in the browser once. Changing `GEMIFI_TLS_HOSTS` makes a new one. Or give your own with `GEMIFI_TLS_CERT` / `GEMIFI_TLS_KEY` (paths inside the container, e.g. under `/config`). The alternative is plain http and an SSH tunnel to the box, so the browser sees `localhost`.
- The image is about 1 GB (192 MB compressed). `.dockerignore` keeps `research/` (copyrighted stock firmware), build outputs, `node_modules` and `./config` out of the build context.

## Tests

```sh
cd tools/builder && python3 -m unittest
```

What the tests check:
- the Keystation YAML generates tables with the board's shape (a small program, `tests/dump_device.c`, compiled with host gcc against `core/include`, prints every field). Plain `make` in `firmware/nuvoton-nuc1xx/midi-controller` and `core/test` generate from that YAML too, so `core/test` checks the generated tables' behaviour on every run.
- `generate` leaves unchanged files alone (same mtime), so `make` doesn't rebuild when the YAML hasn't changed.
- every generated `.c` (the Keystation, every preset, `tests/fixtures/everything-esp32s3.yaml` with all hub types and components, and an empty device) compiles with `gcc -std=c11 -Wall -Wextra -Werror`. The `core/src/*.c` files are compiled against each one, and `settings.c` + `params.c` are linked against it.
- validation errors (file, line, path, message), the CLI, and the dashboard: every API endpoint, per-id output dirs, read-only presets, static serving and traversal, Basic auth, the Host-header rules, the `GEMIFI_*` env vars, and (with `arm-none-eabi-gcc`) a real NUC build through `/api/build`.
