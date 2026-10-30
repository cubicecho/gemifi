# gemifi web updater

A browser page that talks to a gemifi device over Web MIDI SysEx. It shows what the device says
about itself (HELLO), streams its LOG and EVENT messages, reboots it into the bootloader or the
app, and flashes a `.gfw` firmware image. It implements `protocol/sysex.md` at the root of this
repo; change the protocol there first.

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

Web MIDI only works in a **secure context**: HTTPS, or `localhost`. Opening `dist/index.html` from
`file://` does not work; use `npm run preview` to serve a build.

### Browsers

| Browser                       | Works  | Notes                                                                         |
| ----------------------------- | ------ | ----------------------------------------------------------------------------- |
| Chrome, Edge, Opera (desktop) | Yes    | A permission prompt for MIDI devices with SysEx.                              |
| Firefox 108+ (desktop)        | Yes    | See below.                                                                    |
| Android Chrome                | Should | Untested.                                                                     |
| Safari (any)                  | No     | Safari has no Web MIDI. Use another browser, or the host tool (`tools/host`). |

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

1. **Connect.** The page lists MIDI ports, pairing each input with the output of the same name,
   and picks one whose name contains "gemifi" by itself. The device shows up as
   "gemifi boot (…)" in the bootloader and "… (gemifi)" in the app, and is a different port after
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
4. **Log** shows LOG (`[time ms] LEVEL text`) and EVENT messages. **Raw SysEx** switches to every
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
    client.ts          SysexClient: stop-and-wait requests over any Transport
    flash.ts           hello(), reboot(), flashFirmware(): the upload sequence over a DeviceLink
    ports.ts           tells app / bootloader / other ports apart by name
    defaults.ts        tunable timings (reply timeout, retries, reboot wait)
    *.test.ts          Vitest; flash.test.ts runs the whole upload against a simulated device
  midi/
    device-manager.ts  Web MIDI: access, port pairing, hot-plug, the DeviceLink the page uses
  device/              React: hooks and one card per part of the page
  components/, lib/    vendored from the cubeui registry (`npx shadcn add @cubeui/<item>`); not edited
  app.tsx, main.tsx
```

`src/protocol` imports nothing outside itself and no DOM types, so it runs under Node as is
(`node` strips the types): a host tool needs only a `Transport` over its own MIDI library.

## cubeui

Installed from the cubeui registry (`components.json` points `@cubeui` at it):
`page-layout`, `card-layout`, `description-list`, `button`, `confirm-button`, `progress`,
`alert`, `badge`, `code`, `file-picker`, `combobox`, `switch-field` and `tokens`. Re-add an item to
update it; don't edit files under `src/components` or `src/lib` by hand.
