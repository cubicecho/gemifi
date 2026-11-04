# gemifi web updater

A browser page that talks to a gemifi device over Web MIDI SysEx, or over Web Serial for ESP32
and ESP8266 builds without native USB. It shows what the device says about itself (HELLO),
streams its LOG and EVENT messages, reboots it into the bootloader or the app, flashes a `.gfw`
through the gemifi bootloader or an ESP app `.bin` to a device that updates itself, and does the
first install on an ESP chip through its ROM bootloader (esptool-js). It implements
`protocol/sysex.md` at the root of this repo; change the protocol there first.

Later it grows into the config editor (the schema-driven tabs from
[cubicecho/fcb1010](https://github.com/cubicecho/fcb1010)), so it uses the same stack: React,
Vite, TypeScript, and the cubeui components on Tailwind v4.

## Running it

```sh
npm install
npm run dev        # Vite dev server on http://localhost:5173
npm test           # Vitest: the protocol codec, image checks and the upload sequence
npm run build      # static build in dist/
npm run check      # Biome (format, lint) and tsc
```

Served by the gemifi builder (`python3 tools/builder/gemifi_build.py dashboard`, or its Docker
image), the page also gets a **Devices** view: edit a device's YAML, validate and build it, then
hand the built image to the Firmware or first-install card. Validate also shows what the builder read: the
preset slots, DIN MIDI and the extra USB-MIDI ports it adds, and on stm32f1 the DMA1 channel map
(a `dma:` clash is marked "DMA channel"). Under the device, **YAML examples** has copyable examples
of the newer features (XY pad, DIN MIDI, the DMA switches, pitch wheel and aftertouch, LED looks,
presets) from `src/builder/yaml-reference.json`; the builder's tests validate every one of them. The page finds the builder by asking
`api/info`; on GitHub Pages or any static host that request fails and the page is the updater
alone. `npm run dev` proxies `/api` and `/download` to `GEMIFI_BUILDER_URL`
(default `http://127.0.0.1:6055`), so the dev server works against a running builder.

**Presets.** On a device with presets (HELLO caps bit 5) the Config card starts with the active
preset: a picker that asks before dropping unsaved edits, Rename, Save as (into another slot) and
Reset. Settings that belong to a preset are marked "per preset". The page follows switches made on
the device (PRESET_CHANGED) and reads the values again. Save to device stores the active preset
with the globals.

Web MIDI only works in a **secure context**: HTTPS, or `localhost`. Opening `dist/index.html` from
`file://` does not work; use `npm run preview` to serve a build.

### Browsers

| Browser                       | Works  | Notes                                                                         |
| ----------------------------- | ------ | ----------------------------------------------------------------------------- |
| Chrome, Edge, Opera (desktop) | Yes    | A permission prompt for MIDI devices with SysEx.                              |
| Firefox 108+ (desktop)        | Yes    | See below.                                                                    |
| Android Chrome                | Should | Untested.                                                                     |
| Safari (any)                  | No     | Safari has no Web MIDI. Use another browser, or the host tool (`tools/host`). |

**Web Serial** (the Serial transport and the first-install card) is Chrome, Edge and Opera on a
desktop only; Firefox and Safari have none, and the page hides what needs it.

Nothing to install on Linux, Windows or macOS. On Windows before Windows MIDI Services, a MIDI
port can be open in one program at a time, so close DAWs and MIDI monitors first.

### Firefox's permission flow

Firefox gates Web MIDI behind a **site-permission add-on** that it generates for the site:

1. Press **Connect**. Firefox asks whether to allow the site to access MIDI devices.
2. Accept, and Firefox offers to install a small add-on named after the site. Install it. It
   holds the permission only; it has no code.
3. Firefox then shows the usual permission prompt. Allow it.

To take the permission back, remove the add-on in `about:addons`. Each origin (`localhost:5173`,
the GitHub Pages address) gets its own add-on.

## Using it

1. **Connect.** Choose the transport: **USB MIDI** (the Keystation, ESP32-S2/S3 builds with
   native USB) or **Serial (ESP)** (classic ESP32 and ESP8266, whose gemifi build speaks MIDI over
   the board's USB-UART). For Serial, set the baud rate (115200, gemifi's default) and press
   **Choose port**; the browser shows its own port chooser. Opening the port releases RTS before
   DTR, so the board is not reset. The protocol is the same; the SysEx frames are cut out of the
   byte stream and the rest (the ROM boot log, other MIDI) is dropped. Over USB MIDI the page
   lists MIDI ports, pairing each input with the output of the same name, and picks one whose
   name contains "gemifi" by itself. The device shows up as
   "gemifi boot (…)" in the bootloader and "gemifi (…)" in the app, and is a different port after
   every reboot; the page follows it.
2. **Device** asks HELLO on every (re)connection: role, board, firmware and protocol versions,
   max chunk, capabilities and UID. The two buttons send REBOOT(1) and REBOOT(0).
3. **Firmware.** Choose a `.gfw`. The page checks the header (magic, header and protocol versions,
   board id against the connected device, size against the board's APROM), the CRC-32 over
   `[0, size)` with the header's crc field taken as 0, and the vector table the bootloader checks
   (initial SP in SRAM, reset vector odd and inside the image). Nothing is sent until all of that
   passes. **Flash** then reboots into the bootloader if the device is in the app, sends FW_BEGIN,
   the FW_DATA chunks (0x200 upward, page 0 last, never across a 512-byte page), FW_END, reboots
   into the app and checks it answers HELLO with the new version. Each request is stop-and-wait
   with a 200 ms timeout and up to 5 resends with the same `seq`.

   **ESP (A/B) devices.** A device whose app answers HELLO with role 0, the firmware-upload cap
   and a non-zero max chunk takes the upload itself into its inactive OTA slot
   (`protocol/sysex.md` §4.1, §4.2 "A/B devices"); it is never rebooted into a bootloader. Choose
   the build's app `.bin` (not the merged factory image) and type its version (`1.2.3`); the
   board comes from HELLO. The page pads the file with 0xFF to a multiple of 4, sends FW_BEGIN
   with the padded size and the CRC-32 over the padded file, FW_DATA in plain order from offset 0,
   FW_END, then REBOOT(0), and keeps asking HELLO (up to 15 s) until the app reports the new
   version. The old slot keeps working until FW_END has checked the new one.
   **M-Audio Code 61 (board 0x0002).** gemifi has no bootloader of its own there: the app goes in
   through M-Audio's stock bootloader and its SysEx update protocol
   (`research/devices/m-audio-code61/analysis/`, `tools/host/maudio_update.py`). Its `.gfw` has
   the header at 0x130 (76 vectors) and is linked at 0x08002400; the page finds the header at
   whichever offset a known board uses. **Flash** turns the image into M-Audio's record stream (a
   data record per 1 KB page, erase-only records up to 0x0801AFFF, then the 0x0801B000 page with
   the whole-image checksum), sends REBOOT(1) if a gemifi app is connected (it sets M-Audio's
   update request), and asks you to **turn the keyboard off and on**. It then probes the selected
   port and any port that (re)appears with the start message every 2 s: `F0 15 F7` is an app that
   just set the request (power-cycle now), `F0 45 F7` is the bootloader. Each record goes as
   11-byte ASCII-hex SysEx chunks with an ack (10 s timeout) after it; unexpected replies are
   shown in the card. A stock M-Audio update file (`FirmwareFile…hex.txt`) is sent as it is, which
   restores the stock firmware; to use it from a stock keyboard, select its port first. Untested
   on hardware so far.
4. **Install firmware (first flash)**, for an ESP chip with no gemifi firmware yet, or to
   recover one: choose a merged factory image (one `.bin`, written at 0x0, from
   `esptool.py merge_bin` or PlatformIO), or a `manifest.json` in the
   [ESP Web Tools](https://esphome.github.io/esp-web-tools/) format
   (`{ name, version, builds: [{ chipFamily, parts: [{ path, offset }] }] }`) together with its
   part files (matched by file name). **Install** asks for the serial port, connects to the ROM
   bootloader with [esptool-js](https://github.com/espressif/esptool-js) (pinned at 0.7.0, loaded
   only when an install starts), detects the chip, picks the manifest build for it, writes the
   parts with progress, and hard-resets the chip. If the serial transport holds that port, it is
   closed first. A board that does not enter its bootloader by itself needs BOOT (IO0) held while
   RESET is pressed.
5. **Log** shows LOG (`[time ms] LEVEL text`) and EVENT messages. **Raw SysEx** switches to every
   message both ways as hex (`→` sent, `←` received), for debugging the protocol.

## Deploying

The build is static: `npm run build` writes `dist/`, with relative asset paths (`base: './'` in
`vite.config.ts`), so it can be served from any path. GitHub Pages serves over HTTPS, which Web
MIDI needs, and is what fcb1010 uses. A workflow at the repo root would be:

```yaml
# .github/workflows/updater.yml
name: updater
on: { push: { branches: [main], paths: ["web/updater/**", "protocol/**"] } }
permissions: { contents: read, pages: write, id-token: write }
jobs:
  deploy:
    runs-on: ubuntu-latest
    environment: github-pages
    defaults: { run: { working-directory: web/updater } }
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with: { node-version: 24, cache: npm, cache-dependency-path: web/updater/package-lock.json }
      - run: npm ci
      - run: npm test
      - run: npm run build
      - uses: actions/upload-pages-artifact@v3
        with: { path: web/updater/dist }
      - uses: actions/deploy-pages@v4
```

## Layout

```
src/
  protocol/            framework-free, no DOM: reusable by the config editor and a Node tool
    wire.ts            protocol words and numbers: commands, status codes, roles, caps
    encoding.ts        u14 / u28 / u32, 8→7 pack/unpack, text, hex, PayloadReader
    frame.ts           frame encode/decode and the checksum
    messages.ts        request payload builders; HELLO / FW_* reply and LOG / EVENT parsers
    boards.ts          known boards (§7) and their memory sizes
    image.ts           .gfw header, CRC-32, validateImage, planChunks (upload order)
    ab-image.ts        A/B (ESP) images: padding, CRC, version parsing, planLinearChunks, isAbDevice
    stream.ts          SysexStreamParser: whole SysEx messages out of a raw byte stream (serial)
    client.ts          SysexClient: stop-and-wait requests over any Transport
    config.ts          GET_SCHEMA, GET/SET_PARAM(S), COMMIT, FACTORY_RESET
    presets.ts         PRESET_INFO / SAVE / RESET / NAME and selecting a preset (§4.5)
    flash.ts           hello(), reboot(), flashFirmware(), flashAbFirmware(): the upload sequences over a DeviceLink
    ports.ts           tells app / bootloader / other ports apart by name
    defaults.ts        tunable timings (reply timeout, retries, reboot wait)
    *.test.ts          Vitest; flash.test.ts and ab-flash.test.ts run whole uploads against simulated devices
  vendor/
    maudio.ts          M-Audio's update protocol (Code 61): record stream, checksums, ASCII-hex SysEx, probe and upload
  midi/
    device-manager.ts  Web MIDI: access, port pairing, hot-plug, the DeviceLink the page uses
  serial/
    serial-link.ts     Web Serial: the same DeviceLink over a serial port, framed by stream.ts
    esp-manifest.ts    first install: merged image or ESP Web Tools manifest, build and part matching
    esp-install.ts     first install: esptool-js (ROM bootloader) detect, write, hard reset
  device/              React: hooks and one card per part of the page
  components/, lib/    vendored from the cubeui registry (`npx shadcn add @cubeui/<item>`); not edited
  app.tsx, main.tsx
```

`src/protocol` imports nothing outside itself and no DOM types, so it runs under Node as is
(`node` strips the types): a host tool needs only a `Transport` over its own MIDI library.

## cubeui

Installed from the cubeui registry (`components.json` points `@cubeui` at it):
`page-layout`, `card-layout`, `description-list`, `button`, `confirm-button`, `progress`,
`alert`, `badge`, `code`, `file-picker`, `combobox`, `switch-field`, `segmented` and `tokens`. Re-add an item to
update it; don't edit files under `src/components` or `src/lib` by hand.
