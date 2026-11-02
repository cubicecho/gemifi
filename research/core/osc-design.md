# OSC alongside MIDI: design (draft for review)

Written 2026-11-02. Status: **proposal**, nothing implemented. Branch `research/osc`.

The ask: let a gemifi device send and receive **OSC** (Open Sound Control) as well as MIDI, mainly on the ESP32 family, which has Wi-Fi. This doc says what OSC is worth to gemifi users, how it would fit the core without touching the NUC build, and what to build first. Things not verified are marked (unverified).

## TL;DR

- **OSC is worth it on Wi-Fi ESP chips, and only there.** It gives named addresses, more than 7 bits, many hosts at once and no cable or driver. On the USB-only chips (NUC120, STM32F1) there is no network, and OSC over SLIP serial reaches almost nobody. For those, a host-side MIDI → OSC bridge in `tools/host` is the cheap answer.
- **Both at once, not either.** OSC is a second output sink next to MIDI. A global switch says what goes where, and later each control can be routed on its own (`midi`, `osc`, `both`).
- **One small change in the core:** `controller.c` reports what each control did through one call, `gm_out_*()`, instead of building MIDI bytes itself. The MIDI encoder is one sink and `core/src/osc.c` is another. `GM_OSC` is 0 on the NUC, so the NUC build doesn't change.
- **Our own OSC codec, about 300 lines of C,** with no heap and no floating point. A spike shows that OSC float32 values can be built exactly with integer maths (§6.4).
- **The network part lives in the ESP platform:** Wi-Fi station mode with an access-point fallback, UDP, and mDNS. Wi-Fi costs about **650 KB of flash and 25 KB of static RAM** on the ESP32. **BLE plus Wi-Fi fills 91 % of an app slot** before any gemifi code is added (measured, §6.5).
- **Wi-Fi credentials** come from the YAML (`!secret`) at build time and can be changed from the web page over the existing SysEx link (Web Serial or Web MIDI), stored like the profile. Improv-serial can come later, and there is no captive portal.
- **Addresses come from the YAML ids** (`/keys-32/fader/fader_1 f 0.73`). Each control can override its address, which lets it target Reaper or AbletonOSC directly. Addresses and value ranges are *behaviour*, so presets can override them.
- **Incoming OSC on the same address is feedback.** It mirrors the existing DAW feedback design: LEDs, toggle state, encoder value and pick-up.
- **Config over the network comes later and is off by default.** Browsers can't send UDP, so the web editor would need a WebSocket on the device, and an https page can't open a plain `ws://` link to a LAN address. Updating over the network needs a password.

---

## 1. What OSC adds, and for whom

### 1.1 Over MIDI

| | MIDI (today) | OSC |
|---|---|---|
| Resolution | 7-bit CC (0..127); pitch bend 14-bit | 32-bit int or float. The ADC path already has 12 bits (`analog_poll` keeps `pos` 0..4095 and throws away 5 bits for the CC) |
| Naming | channel + number; the user maps "CC 20" in the host | an address the user can read, `/keys-32/fader/fader_1` |
| Hosts | one USB/BLE host at a time | any number of hosts on the LAN, by unicast, broadcast or several targets |
| Cable / driver | USB cable or BLE pairing | none: Wi-Fi. Phones and tablets (TouchOSC, Lemur) take it with no adapter |
| Message shape | fixed 2–3 bytes | any number of typed arguments (`/note i 60 f 0.8`) |
| Timing | USB: sub-ms and steady | Wi-Fi: usually a few ms, with jitter spikes of tens of ms (§2.4) |
| Host support | every DAW | patchers and show tools first; DAWs only partly (below) |

### 1.2 Who would use it

| Host | OSC | Notes |
|---|---|---|
| Max, Pure Data, SuperCollider, VCV Rack (plugins: cvOSCcv, trowaSoft) | native / easy | the core audience: people who want named, high-resolution controls |
| TouchOSC, Open Stage Control, Chataigne, ossia score | native | apps often used as OSC hubs; ossia and Chataigne also speak OSCQuery |
| Resolume, QLab, TouchDesigner, MadMapper, Millumin | native | VJ, theatre and installation work, which tends to be on Wi-Fi anyway |
| REAPER, Ardour | native, with a pattern file (`.ReaperOSC`) | the address has to be what REAPER expects (`/track/1/volume f`), so per-control addresses matter (§3.3) |
| Ableton Live | not built in: Max for Live (Connection Kit "OSC Send/Receive") or AbletonOSC (a Remote Script) | AbletonOSC addresses take extra leading args (`/live/track/set/volume i 0 f 0.7`) |
| Bitwig | not built in: the DrivenByMoss extension (unverified for the current release) | |
| Logic, Cubase, FL Studio | no OSC that a controller can use (unverified for Cubase) | MIDI stays the way into these |

The people who would use it are DIY ESP32 builders who make installations, live visuals, theatre cues or patcher instruments, and people who want a wireless controller for a phone, tablet or second laptop without BLE-MIDI pairing. People using a DAW keep MIDI: it has DAW feedback and Mackie Control, and it works in every DAW.

### 1.3 Where it doesn't make sense

- **NUC120 (Keystation) and STM32F103 (Code 61): there's no network.** OSC over USB would mean SLIP-framed OSC on a CDC serial port. That needs a second USB class on a 4 KB-RAM chip, it doesn't work with the Web MIDI updater, and only Max, Pd, SuperCollider and a few others can read SLIP. **Recommendation: don't do it.**
- **For USB devices, a host bridge instead.** `tools/host/gemifi_osc_bridge.py` would read the device's MIDI and its schema (names, CC numbers), then send named OSC: `/keystation-mini32/analog/Volume f 0.5`. It costs nothing on the device and works with every gemifi device today. The resolution stays 7-bit. It's cheap enough to build early (Phase 1b).
- **Wi-Fi is not a replacement for USB when timing matters** (a velocity keybed played live into a DAW). Say so in the docs.

## 2. Network on the ESP chips

### 2.1 Chips

| Chip | Wi-Fi | Fit for OSC | Notes |
|---|---|---|---|
| ESP32-S3 | yes | **best** | USB MIDI and OSC at once; BLE too, but see the flash budget (§6.5) |
| ESP32 (classic) | yes | good | serial or BLE MIDI plus OSC |
| ESP32-S2 | yes | good | no BLE, and **no FPU** (the integer float encoder in §6.4 covers it) |
| ESP8266 | yes (2.4 GHz, b/g/n) | possible, phase 5 | Wi-Fi is always linked in, so OSC costs about 27 KB of flash. 80 KB of RAM. The scan runs from `loop()`, so the radio adds scan jitter. No BLE |
| NUC120, STM32F1 | – | no | §1.3 |

**Ethernet (W5500 over SPI)** would give steady latency for fixed installations. Arduino-ESP32 3.x has an `ETH` driver for the W5500 (unverified: check which core version added it). The design keeps the network behind one HAL, so Ethernet is one more platform file. It's phase 5, and only if someone asks.

### 2.2 Wi-Fi mode

- **Station (STA) is the default:** the device joins the user's network.
- **Fallback access point (AP):** if STA hasn't connected within `ap_timeout` (default 30 s), the device starts its own AP `gemifi-<name>` (password from the YAML, or open), as ESPHome's `ap:` does. A phone can join it and run TouchOSC at a show with no router. AP-only mode is a YAML option (`wifi: { mode: ap }`).
- **Power saving:** turn **modem sleep off** (`WiFi.setSleep(false)` / `WIFI_PS_NONE`). With the default modem sleep, Espressif say packets to the device can be held for up to about 300 ms. This costs about 100 mA all the time (unverified, typical ESP32 numbers), which a USB-powered controller doesn't mind. A battery build can choose `power_save: light` and accept the latency.

### 2.3 Getting the Wi-Fi credentials onto the device

| Way | Effort | UX | Verdict |
|---|---|---|---|
| YAML at build time (`ssid: !secret wifi_ssid`) | small: builder | fine for DIY builders, who build anyway | **phase 1** |
| The web page, over the link it already uses (Web Serial / Web MIDI, SysEx) | medium: a string "blob" in the protocol (§5.1) | good: the same page that flashes the device asks for the network | **phase 2** |
| Improv Wi-Fi over serial | small–medium: a second framed protocol on UART0 | ESP Web Tools shows "connect to Wi-Fi" right after the first flash | **later, maybe.** It clashes with `midi.transport: serial` on UART0, which reads the same bytes. It only helps on Web Serial boards |
| Improv over BLE | medium | phone-friendly | no: it costs BLE flash just for setup |
| Captive portal (AP plus a web form) | large: HTTP server, DNS, HTML in flash | works with no other tool | **no.** It adds 100+ KB, and our web page already does the job |

The credentials are stored in NVS next to the profile (a key of their own, so FACTORY_RESET of the profile can keep them; Q7). Like the profile, they are never inside the app slots, so an OTA update keeps them.

### 2.4 UDP, targets and discovery

**Sending.** One UDP socket. OSC messages go to the configured targets:

| `osc.target` | Meaning |
|---|---|
| `{ host: 192.168.1.20, port: 9000 }` | fixed unicast (phase 1) |
| `{ host: mypc.local, port: 9000 }` | resolved by mDNS at connect time, then re-resolved when sends start failing |
| `{ host: broadcast, port: 9000 }` | the subnet broadcast. It's simple and reaches every host, but Wi-Fi sends broadcasts at the lowest basic rate, so it's slower (unverified on the ESP32; measure it) |
| `reply` (the default when no target is set) | send to whoever last sent us OSC, the way TouchOSC and many hardware boxes do. A host "subscribes" just by sending anything |

Up to 4 targets (Q4). Each is about 8 bytes of RAM.

**Receiving.** `osc.listen_port` (default 8000) receives feedback (§3.5) and pings.

**mDNS.** The device advertises `<name>.local` and the services `_osc._udp` (port = `listen_port`, TXT `node=<name> board=<id> fw=<version>`), plus `_gemifi._udp` once config over the network exists (§5). mDNS costs about 37 KB of flash on the ESP32 (measured). The builder dashboard could list devices it finds, as the ESPHome dashboard does.

**Latency and jitter.** USB MIDI is sub-millisecond and steady. On Wi-Fi with modem sleep off, people report about 1 ms from send to receive on a quiet network, with spikes of tens of ms when the air is busy (ESP32 forum reports, not measured here). OSC can't make this steadier: its timetags would need a shared clock (NTP or similar), and the host would have to buffer. Phase 0 measures it on the S3, with a round-trip ping at `/gemifi/ping`.

**Packet rate.** `gm_poll` can produce several changes in one pass (a chord, moving several faders). Each one gets its own datagram in phase 1. A later option, `osc.bundle: true`, packs one `gm_poll` pass into one bundle with the timetag "immediately" (1), which cuts the packet count. Phase 1 needs a rate limit for analog inputs anyway: at most one send per control every `osc.min_interval` (default 10 ms), always sending the last value.

### 2.5 BLE coexistence

The ESP32 and S3 share one 2.4 GHz radio between Wi-Fi and BLE in time slices. Both work, but BLE-MIDI latency and Wi-Fi throughput both get worse (unverified by how much). The bigger problem is flash: BLE (Bluedroid) plus Wi-Fi plus mDNS is **1.79 MB of a 1.97 MB app slot**, before any gemifi code (§6.5). So:
- The builder **warns** on `midi.transport: ble` together with `wifi:`. It **refuses** the combination if the image size check after the build fails.
- A way out is NimBLE instead of Bluedroid. It's much smaller (unverified: whether Arduino-ESP32 3.3 can build its BLE library on NimBLE, or whether we use the NimBLE-Arduino library). That's a separate change to the BLE transport.

## 3. Address and value model

### 3.1 Default addresses

`/<node>/<kind>/<id>[/<sub>]`. `<node>` is the YAML `name` (`keys-32`), `<kind>` is the component list and `<id>` is the component's YAML id. Ids are already lower_snake_case and unique, so they're safe in OSC addresses. Today ids exist only at build time. With `osc:`, the builder writes each address string into flash: about 20–30 B per control, which is nothing on an ESP.

| Control | Address | Args | Notes |
|---|---|---|---|
| analog | `/keys-32/analog/fader_1` | `f` 0.0..1.0 | from the 12-bit position (0..4095). `range: int` sends `i` 0..4095 instead, and `range: midi` sends `i` 0..127 |
| button (any action) | `/keys-32/button/sustain` | `i` 1 / 0 | pressed / released, whatever the action. TouchOSC-style hosts want `f` 1.0 / 0.0: `osc.bool: float` |
| `cc_toggle` button | `/keys-32/button/mute` | `i` 1 / 0 | the toggle **state**, not the press |
| `cc_ramp` button | `/keys-32/button/swell` | `f` 0..1 | the ramp value as it glides |
| key (matrix) | `/keys-32/key` | `i` note, `f` velocity 0..1 (0 = release) | one address for the whole keybed, the way MIDI sends notes. `osc.keys: per_key` gives `/keys-32/key/60 f 0.8` instead (Q5) |
| `note` button | `/keys-32/key` | as keys | it's a note, so it goes with the keys |
| encoder | `/keys-32/encoder/pan_1` | `i` delta (±n) | relative by default. `encoder.mode` absolute sends `i` 0..127 or `f` 0..1, from the same accumulated value MIDI uses |
| pitch bend (bend buttons) | `/keys-32/bend` | `f` −1..1 | |
| octave / transpose / program | `/keys-32/octave` `i`, `/keys-32/transpose` `i`, `/keys-32/program` `i` | | sent when they change |
| device | `/keys-32/hello` | `s` friendly name, `i` board id, `s` version | at boot and in answer to `/keys-32/ping` or `/gemifi/ping` |

Unnamed controls are not a problem: the builder makes an id for every component.

### 3.2 Values and resolution

- The analog path already filters to 12 bits. OSC sends `pos / 4095` (or `pos`) with its own small deadband (±2 of 4095, Q6), not the 7-bit hysteresis that the CC needs. A noisy pot would otherwise flood the network, so the deadband plus `min_interval` keeps the rate sane.
- Velocity: the keybed path computes 1..127 today. OSC sends `v / 127` in phase 1. When the presets work adds velocity curves, the curve output could carry more bits (Q6).
- Floats come from the integer encoder (§6.4), not from `float` maths.

### 3.3 Per-control addresses (and why they matter)

Many OSC hosts have fixed addresses (REAPER, AbletonOSC, QLab, Resolume). A control can name its own:

```yaml
analog:
  - id: fader_1
    pin: { cd74hc4051: faders, number: 0 }
    cc: 20
    osc:
      address: /track/1/volume              # REAPER's default pattern
  - id: fader_2
    pin: { cd74hc4051: faders, number: 1 }
    osc:
      address: /live/track/set/volume       # AbletonOSC
      args: [1, $value]                     # fixed leading args, then the value
buttons:
  - id: go
    pin: GPIO9
    action: none                            # nothing on MIDI
    osc: { address: /go, on_press_only: true }   # QLab "go"
```

- `address`: a literal path. `{node}` and `{id}` expand at build time, so `/{node}/x/{id}` is the default template.
- `args`: fixed `i`/`f`/`s` args with `$value` where the control's value goes (at most 4, all strings in flash).
- `range`: `[min, max]` (floats) maps the value onto that range, e.g. REAPER pan `[-1, 1]`.

### 3.4 Presets compatibility

The presets draft (`feat/presets`, `research/core/presets-design.md`) uses the rule "hardware side is global, mapping is per preset". **OSC addresses, args, ranges and the route are mapping**, so they are per-preset behaviour. The pins and the Wi-Fi settings stay global.

```yaml
presets:
  list:
    - name: Default
    - name: REAPER
      analog:
        fader_1: { osc: { address: /track/1/volume } }
        fader_2: { osc: { address: /track/2/volume } }
    - name: Visuals
      osc: { route: osc }                     # global route for this preset (§4.2)
      analog:
        fader_1: { osc: { address: /composition/layers/1/video/opacity } }
```

Params are `s32` only, so strings don't fit them. The proposal:
- **Strings live in flash only, from the YAML.** The builder makes one address table per preset (sparse, like the presets draft's override table). The live params are just numbers: `osc.route` (per control: 0 midi, 1 osc, 2 both, 3 none) and `osc.range` (enum).
- **Changing an address from the web page** would need string params. That goes with the string/blob protocol work (§5.1) and isn't in the first phases. Until then, a new address means a rebuild, as a new pin does today. (Q8)

### 3.5 Incoming OSC: feedback

Incoming OSC mirrors F1 plain feedback (core/README.md "DAW feedback"). **A message to a control's own address sets its state**, so a host that echoes values back (TouchOSC, Open Stage Control, REAPER's feedback) keeps the device in sync:

| On the device | Incoming message | Does what MIDI feedback does |
|---|---|---|
| `cc_toggle` button | its address, value ≥ 0.5 / ≥ 1 | sets the toggle state (and its LED) |
| button with an LED | its address | lights the LED (host level, owned per `owner:` / `led_timeout`) |
| LED | `/<node>/led/<id> i 0/1/2` or `f` | 0 off, 1 blinking, 2+ on, the MCU palette as now. Any LED, with or without a button |
| encoder, absolute | its address | becomes the value the next turn continues from |
| analog with `takeover: pickup` | its address | becomes the pick-up target, at 12 bits instead of 7 |
| `/gemifi/ping`, `/<node>/ping` | – | reply `/<node>/hello`; with `target: reply` this is also the subscribe |

- The calls exist already: `gm_led_host(led, value)` and `gm_analog_host(i, value)` (gm_feedback.h). Toggle and encoder sync are inside `gm_fb_msg` today, keyed by CC. They become two small functions, `gm_button_host(i, on)` and `gm_encoder_host(i, value)`, which both MIDI and OSC feedback call. MIDI behaviour doesn't change.
- `feedback: { enabled: false }` turns OSC feedback off too. Clock and transport LEDs stay MIDI-only. An OSC "transport" could be an LED source later (`feedback: { osc: /play }`, Q9).
- **Matching:** exact string match against the device's address table, which is fast enough for a few hundred addresses with a hash or a sorted table. OSC wildcards (`*`, `?`, `[ ]`, `{ }`) in incoming addresses come later. Hardly any host sends them.
- **Bundles in:** unpacked and handled in order. Timetags are ignored: everything happens now.
- **Arguments in:** `i`, `f`, `T`/`F` (true/false) and `d` (cast) are accepted. Anything else is ignored with a debug log.

## 4. "Alongside MIDI": routing and the core

### 4.1 Both at once

With `osc:` in the YAML, the device speaks OSC **and** MIDI, and the SysEx config link stays on MIDI. A device with only OSC is still a gemifi device: it keeps a MIDI transport (USB or serial) for config and recovery.

### 4.2 Routing

| Level | YAML | Param | Phase |
|---|---|---|---|
| global | `osc: { route: both }`: `midi` \| `osc` \| `both` | `osc.route` (global, per preset) | 1 (fixed at build) / 2 (param) |
| per control | `osc: { route: osc }` on a button, analog or encoder | `button.route`, `analog.route`, `encoder.route` | 3 |

`midi` means OSC sends nothing for that control (OSC feedback still works). `osc` means no MIDI message for it. Keys follow the global route (Q5).

### 4.3 The output layer

Today `controller.c` turns each change into MIDI bytes right where it happens (`cc()`, `midi3()`, `midi2()`), and calls `event()` for the SysEx EVENT debug stream. That stream already carries the transport-neutral facts, with the 14-bit analog value. The proposal makes those facts the one output and turns MIDI into a sink:

```
controller.c ── gm_out_*(kind, index, …) ──┬─ out_midi.c  (today's cc()/midi3(): channel, numbers from gm_cfg)
   (scan, actions, ramps, encoders)        ├─ osc.c       (GM_OSC: address table, encode, hal_net_send)
                                           └─ app_event() (EVENT debug stream, as now)
```

- **The calls** (in a new `core/include/gm_out.h`):
  - `gm_out_button(i, state)`
  - `gm_out_cc(src, i, num, v7, v12)`, where `src` is the control kind
  - `gm_out_note(src, i, note, vel, on)`
  - `gm_out_bend(v14)`
  - `gm_out_program(p)`
  - `gm_out_encoder(i, delta, abs)`
  - `gm_out_global(id, value)`, for octave and transpose
  
  Each call carries both the MIDI-ready value and the full-resolution one.
- **MCU stays a MIDI-only path.** `gm_mcu_fader` and friends are called as now. OSC doesn't see MCU-assigned controls unless it's given its own route (Q10).
- **NUC cost:** with `GM_OSC 0`, `gm_out_*` are `static inline` functions in the header that do exactly what `cc()`/`midi3()` do now, so the NUC image should be byte-for-byte the same or within a few bytes. Check it with the size line in `make` and the existing `core/test` vectors.
- **Platform side (ESP only):** a new `platform/esp/net_esp.cpp` with:
  - `hal_net_send(target, buf, n)`, which queues;
  - `hal_net_poll()`, called from `gm_esp_loop()`: Wi-Fi state, receive, `gm_osc_rx(buf, n, src)`;
  - Wi-Fi bring-up, AP fallback and mDNS.
  
  The scan task never touches the network: everything happens in `loop()`, as MIDI does now.
- **`core/src/osc.c`** (pure C11, host-testable): it encodes into a static 256 B buffer, decodes in place from the receive buffer, matches against the generated `gm_osc_addr[]`, and calls the feedback functions. It has no heap and no float. It uses `uint64_t` division in the float encoder, which is fine on the ESP.

## 5. Config and updates over the network

### 5.1 Strings in the protocol

Wi-Fi credentials, target hosts and (later) addresses are strings, but every param is an `s32` today. `protocol/sysex.md` reserves `0x05 GET_BLOB` / `0x06 SET_BLOB`, and they fit this: **a blob is a small id-addressed byte string**, with an id space like the param ids (`1 wifi.ssid`, `2 wifi.password` write-only, `3 osc.target_host`, …). The schema lists the blobs with a new entry kind, so the editor can render text fields. Old editors skip it if it's a separate section after the params, so `schema_version` stays 1 (check that `check_schema.py` and the web parser agree, Q11). The password can be written but never read back.

### 5.2 Reaching the config protocol over Wi-Fi

| Option | Device cost | Who can use it | Notes |
|---|---|---|---|
| **A. SysEx frames in UDP** (`_gemifi._udp`, one frame per datagram, the same bytes as the gsx frame) | tiny: feed `app_proto_rx_byte`, reply on the same transport | `gemifi.py --udp host`, the builder dashboard | stop-and-wait with retries already handles UDP loss. **But browsers can't send UDP** |
| **B. WebSocket** on the device (the same frames, binary) | an HTTP/WS server, ~30–60 KB (unverified) | the web page directly | **mixed content:** an https page can't open `ws://192.168.x.y`. Chrome's Private Network Access rules add more hurdles. It works from `http://localhost` (the dashboard) or from a page the device serves itself |
| **C. The dashboard as a bridge** (browser ↔ builder over HTTP/WS ↔ UDP to the device) | as A | the web page, when it's served by the builder | no TLS problem: the page and the bridge share an origin |
| **D. OSC as the carrier** (`/gemifi/sysex b <frame>`) | as A | OSC tools | fun, but nobody needs it. A is the same thing without the OSC wrapper |
| **E. OSCQuery** (HTTP + JSON tree of the address space, `_oscjson._tcp`) | an HTTP server plus JSON | ossia score, Chataigne, some VRChat tools (unverified list) | describes **OSC addresses**, not our params, ranges, enums, groups or presets. It doesn't replace the schema |

**Recommendation:** the reply path in `app_proto.c` gets a "reply to the transport the request came from" (today it always calls `hal_midi_send`). Then **A** (phase 4), with **C** for the web page. **B** waits until we decide whether the device should serve its own page (Q12). **E** could be a static JSON file the builder writes from the address table and the device serves read-only. It's optional and cheap if B's HTTP server exists anyway.

### 5.3 Security

OSC has no security: anyone on the LAN can send `/keys-32/led/...`. That's normal and fine for control. Config and firmware are different:

- **Network config is off by default** (`wifi: { config: false }`). When it's on, reads are open and writes need the password.
- **A password for SET_*, COMMIT, FACTORY_RESET and FW_*** (`wifi: { password_config: !secret gemifi_pw }`), like ESPHome's `ota: password`. On UDP it's a challenge–response: the device sends a nonce, the host returns HMAC-SHA256(key, nonce), with mbedTLS on the ESP. It isn't encrypted, but it doesn't send the password in plain text and it stops replays. This is a new command pair (Q13).
- **Firmware over the network** also needs the image's `board_id` to match, as now. It's the same A/B slot path, so a bad upload can't brick the device.
- Over USB/serial nothing changes: physical access counts as trust, as today.
- The AP fallback should have a password unless the user sets `ap: { password: "" }` on purpose.

## 6. YAML, builder, code, cost

### 6.1 YAML sketch

```yaml
# secrets.yaml (next to the device file, gitignored), ESPHome style
wifi_ssid: StudioNet
wifi_password: hunter2
gemifi_pw: 3x4mpl3
```

```yaml
gemifi: 2
name: keys-32
platform: esp32s3
chip: esp32-s3-devkitc-1
midi: { transport: usb }

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap: { password: !secret wifi_password, timeout: 30s }   # fallback AP gemifi-keys-32; omit: no AP
  power_save: none                 # none (default) | light
  # manual_ip: { static_ip: 192.168.1.50, gateway: 192.168.1.1, subnet: 255.255.255.0 }
  config: false                    # true: the SysEx protocol over UDP too (§5), needs password_config
  # password_config: !secret gemifi_pw

osc:
  route: both                      # midi | osc | both (default both)
  listen_port: 8000
  targets:                         # default: [reply]
    - { host: studio-mac.local, port: 9000 }
    - reply
  bool: int                        # int (1/0) | float (1.0/0.0)
  keys: note                       # note: /keys-32/key i note f vel | per_key: /keys-32/key/<n> f vel
  min_interval: 10ms               # per-control rate limit for continuous values
  bundle: false                    # true: one bundle per poll pass
  mdns: true                       # advertise keys-32.local and _osc._udp

analog:
  - id: fader_1
    pin: { cd74hc4051: faders, number: 0 }
    cc: 20
    osc: { address: /track/1/volume }      # optional; default /keys-32/analog/fader_1
```

`!secret` is resolved by the builder from `secrets.yaml` next to the device file, then the config dir root, as ESPHome does. The dashboard's config volume holds it. Built images contain the plain credentials, so the builder never puts them in logs, and the image download says so.

### 6.2 Builder validation

- `wifi:` and `osc:` only on platforms with Wi-Fi (esp32, esp32s2, esp32s3, esp8266). On the NUC and STM32 it's an error naming the platform. The `EspPlatform` table gets a `wifi=True` flag.
- `osc:` without `wifi:` (and later without `ethernet:`) is an error.
- `ssid` 1–32 bytes, password 8–63 or empty (WPA2 rules). A missing `!secret` key gives an error naming the key and the files searched.
- Addresses must start with `/` and contain no spaces or `#`, and they must be unique per preset among the inputs (two faders on one address is a warning: it's allowed but probably a mistake). `args` can hold at most 4 entries and exactly one `$value`.
- Ports 1–65535. `listen_port` must not be the same as a target port on `reply` (warning).
- `midi.transport: ble` plus `wifi:` is a warning with the flash numbers (§2.5). The post-build size check stays the final word.
- ESP8266 plus `osc:`: a warning that the scan runs from `loop()` and Wi-Fi adds jitter.

### 6.3 Generated code

```c
/* gm_device_config.h */
#define GM_OSC 1
#define GM_WIFI 1
#define GM_OSC_LISTEN_PORT 8000
#define GM_OSC_N_TARGETS 2
#define GM_OSC_BOOL_FLOAT 0
#define GM_OSC_KEYS_PER_KEY 0
#define GM_OSC_MIN_INTERVAL 10          /* ms */
#define GM_WIFI_CONFIG 0
```

```c
/* gm_device_config.c */
const char gm_wifi_ssid[] = "StudioNet";           /* build-time default; NVS wins */
const char gm_wifi_pass[] = "hunter2";
const gm_osc_target_t gm_osc_targets[] = { { "studio-mac.local", 9000 }, { NULL, 0 } };  /* NULL = reply */
const gm_osc_addr_t gm_osc_addr[] = {                /* one per output, sorted for lookup */
    { GM_OUT_ANALOG, 0, "/track/1/volume", 0 },
    { GM_OUT_BUTTON, 0, "/keys-32/button/sustain", 0 },
    /* ... */
};
/* only with presets that override osc: */
const gm_osc_override_t gm_osc_preset_2[] = { { GM_OUT_ANALOG, 0, "/track/2/volume" } };
```

### 6.4 Library choice

| Library | Fit |
|---|---|
| CNMAT OSC (Arduino) | complete, but C++ with dynamic `OSCMessage` objects (heap) |
| ArduinoOSC (hideakitai) | easy API, but it pulls in its own containers and callbacks (unverified: heap use), and it's Arduino-only |
| MicroOsc (T. Fredericks, 0.1.x, 2024) | small, UDP and SLIP, stateless. Arduino C++ and float-based, little error checking |
| **our own `core/src/osc.c`** | **chosen.** OSC 1.0 encoding is padding to 4 bytes plus big-endian ints. We need only `i f s T F b` and bundles in. Our own code follows the core's rules (C11, no heap, no float), runs in `core/test` on the host, and is about 300 lines |

**Spike: floats without floating point.** The core avoids `float`. The S2 and ESP8266 have no FPU, and a soft-float library costs flash. `research/core/osc-spike/float_bits.py` builds IEEE-754 single bits for `num / den` with integer ops only (normalise, a 64-bit divide, round half to even), written the way the C would be. It matches `struct.pack('>f', …)` for **all 87,274 values** of `num/den` with `den` ∈ {1, 2, 100, 127, 1023, 4095, 16383, 65535}. The C version is about 20 lines. (The reference rounds through a double first; at these sizes that never differs from a direct rounding, and there were no mismatches.)

Network stack: `WiFi.h` / `WiFiUdp` / `ESPmDNS` from Arduino-ESP32, and `ESP8266WiFi` / `ESP8266mDNS`. There's no async UDP library: `loop()` polls the socket, which suits the current one-loop design.

### 6.5 Flash and RAM (measured)

Measured 2026-11-02 with a throwaway sketch, `min_spiffs.csv` slots (PlatformIO's `espressif32` platform with `framework-arduinoespressif32 @ 4.30312.261024`, i.e. Arduino-ESP32 3.3.x, not the repo's pinned pioarduino zip, which needs a newer PlatformIO Core than is installed here; ESP8266 Arduino core, d1_mini). These are deltas against an empty sketch, not the gemifi image.

| Build | Flash | Static RAM |
|---|---|---|
| ESP32, empty | 287 KB | 22 KB |
| ESP32 + Wi-Fi + UDP | 933 KB (**+646 KB**) | 47 KB (+25 KB) |
| ESP32 + Wi-Fi + UDP + mDNS | 970 KB (+37 KB for mDNS) | 49 KB |
| ESP32 + BLE (Bluedroid) | 1,179 KB (+892 KB) | 41 KB |
| **ESP32 + BLE + Wi-Fi + mDNS** | **1,792 KB = 91 % of the 1,966 KB slot** | 65 KB |
| ESP32-S3, empty → + Wi-Fi + mDNS | 289 → 930 KB | 22 → 49 KB |
| ESP8266, empty → + Wi-Fi + UDP + mDNS | 265 → 292 KB (+27 KB; Wi-Fi is always linked) | 28.1 → 28.4 KB |

Wi-Fi also takes about 40–60 KB of heap at runtime for its buffers (unverified; check `ESP.getFreeHeap()` in phase 0). gemifi's own OSC code should be under 8 KB of flash. The address table is about 30 B per control. With USB or serial MIDI, OSC fits easily on every ESP32. With BLE MIDI on a 4 MB board it doesn't fit comfortably (§2.5).

## 7. Phased plan

| Phase | What | Done when |
|---|---|---|
| **0. Spike** (S3 dev board) | Wi-Fi STA from `!secret`, send `/gemifi/ping` round trips to `tools/host/gemifi_osc.py`, log latency and jitter with sleep on and off, and free heap | numbers in this doc; go / no-go on the defaults |
| **1. OSC out, fixed host** | `gm_out.h` refactor (NUC unchanged), `core/src/osc.c` encoder, `platform/esp/net_esp.cpp` (STA, UDP send), YAML `wifi:` + `osc: { targets, route }` with default addresses, builder checks, `!secret` | a fader on an S3 moves an Open Stage Control or Pd slider with 12-bit resolution while USB MIDI keeps working |
| **1b. Host bridge** | `tools/host/gemifi_osc_bridge.py`: MIDI + schema names → OSC, for the Keystation and every USB device | the Keystation drives TouchOSC or Pd by name |
| **2. Feedback and discovery** | OSC in (decoder, address match, `gm_button_host` / `gm_encoder_host`), `/led/<id>`, `target: reply`, mDNS, AP fallback, Wi-Fi credentials from the web page (GET/SET_BLOB, §5.1) | TouchOSC sends a toggle and the device LED follows; set up from the web page with no rebuild |
| **3. Per-control addresses and presets** | `osc: { address, args, range, route }` per control, per-preset overrides, per-control route params | a REAPER preset and a Resolume preset on the same box |
| **4. Config over the network** | the reply-transport change, SysEx-in-UDP, HMAC auth, `gemifi.py --udp`, the dashboard bridge, OTA over Wi-Fi | the web page served by the dashboard configures and updates a device with no USB cable |
| **5. Extras, on request** | ESP8266, W5500 Ethernet, bundles out, OSC wildcards, OSCQuery JSON, NimBLE so BLE + Wi-Fi fit, a WebSocket on the device | |

### Tests

- **`core/test/test_osc.c`** (host, like `test_feedback.c`):
  - encoding vectors for each message kind (byte-exact, with the 4-byte padding cases);
  - the integer float encoder against a table generated by the spike script;
  - decoding of messages and nested bundles, including bad sizes, odd padding and unknown type tags;
  - feedback dispatch (address → LED level, toggle, pick-up).
  
  Run under the same `-Wall -Wextra -Werror` flags as the rest of `core/test`.
- **NUC size guard:** the Keystation build's `.text`/`.data` must not grow when `GM_OSC` is 0 (a check in `core/test` or CI).
- **`tools/host/gemifi_osc.py`:** a UDP OSC receiver and sender with no dependencies (our own codec, so the test vectors come from one place, with `python-osc` as a cross-check in tests only). It has a `monitor` command that prints messages, and `ping` / `send /addr value` for feedback tests. `make_test_vectors.py` writes OSC vectors for C and Python together, as it does for SysEx.
- **`tools/host/tests/sim_device.py`** grows a UDP mode for phase 4, so the protocol tests run against SysEx-in-UDP too.
- **Builder tests:** YAML cases for every new error and warning (no Wi-Fi platform, missing secret, duplicate address, BLE + Wi-Fi).
- **Hardware:** an S3 board against Open Stage Control and Pd on Linux, and TouchOSC on a phone. For latency, use the phase 0 ping on an idle and on a busy network.

## 8. Questions to settle

1. **Do we do OSC at all, and on which chips?** Recommendation: yes, on ESP32 / S2 / S3. ESP8266 only after phase 3, and no OSC over USB/SLIP on the NUC or STM32.
2. **A host bridge for USB devices (phase 1b)?** Recommendation: yes. It's cheap, it gives every existing gemifi device named OSC today, and it tests the address scheme before any firmware does.
3. **Both at once, or either?** Recommendation: both. A global `route` in phase 1 and per-control routes in phase 3. MIDI stays on, because the config link needs it.
4. **Targets:** a fixed host, mDNS name, broadcast, `reply`, or all of them? Recommendation: fixed host and `reply` in phase 1–2, mDNS names in phase 2, broadcast only if asked for. At most 4 targets.
5. **Keys:** one address with the note as an argument (`/node/key i 60 f 0.8`), or one address per key (`/node/key/60 f 0.8`)? Recommendation: the first as the default, because it's what patchers expect and keeps the table small, with `keys: per_key` as an option.
6. **Value format defaults:** analog `f` 0..1, buttons `i` 1/0, velocity `f` 0..1? Recommendation: yes, with `bool: float` for TouchOSC-style hosts and `range:` per control. The analog deadband is ±2 of 4095. The higher-resolution velocity waits for the presets work's curves.
7. **Wi-Fi credentials and FACTORY_RESET:** should a factory reset also wipe Wi-Fi? Recommendation: no, so a remote device doesn't drop off the network. A separate "forget Wi-Fi" on the web page, and the safe-mode boot combo starts the AP.
8. **Editable addresses at runtime:** do addresses need to be editable from the web page (string params), or are YAML-only addresses enough? Recommendation: YAML-only until phase 3 is in use. Then decide based on whether people want to retarget without a rebuild.
9. **OSC as an LED source** (`feedback: { osc: /play }`), so any address can drive any LED? Recommendation: phase 3. `/node/led/<id>` covers most cases first.
10. **MCU-assigned controls on OSC:** should a fader with `mcu: fader_1` also send OSC? Recommendation: no by default. A per-control `osc: { route: both }` can turn it on.
11. **Blob/string support in the protocol** (GET/SET_BLOB, a schema section for text fields): is this the way, or a new schema param type `string`? Recommendation: blobs, since they're already reserved and don't change `schema_version`. Write it in `protocol/sysex.md` first, as the repo rule says.
12. **Should the device serve its own web page** (which would give a WebSocket and maybe OSCQuery)? Recommendation: not now. The dashboard bridge (§5.2 C) covers the editor without an HTTP server on the device. Revisit after phase 4.
13. **Network security:** is the HMAC challenge–response for writes and updates enough, with network config off by default? Recommendation: yes for a LAN device, and TLS is out of scope. Ask whether "reads open, writes need a password" is right, or whether reads should need the password too.
14. **BLE + Wi-Fi together:** refuse, warn, or move BLE to NimBLE? Recommendation: warn plus the post-build size check now, and the NimBLE move as its own task if someone wants both.
15. **Naming:** `/<node>/<kind>/<id>`, or a fixed `/gemifi/...` root? Recommendation: `<node>` (the YAML `name`), so two devices on one network don't collide and patches read naturally. `/gemifi/ping` is the one fixed address, for discovery.

## Sources

- OSC 1.0 spec (opensoundcontrol.org) and OSC 1.1 (NIME 2009 paper) for the encoding, bundles and type tags.
- OSCQuery proposal: github.com/Vidvox/OSCQueryProposal (HOST_INFO, `_oscjson._tcp`).
- Improv Wi-Fi serial: improv-wifi.com; ESPHome `improv_serial`.
- MicroOsc: github.com/thomasfredericks/MicroOsc (0.1.7, 2024-02).
- AbletonOSC: NIME 2023 paper "AbletonOSC: A unified control API for Ableton Live".
- ESP32 modem-sleep latency: esp32.com forum threads (Espressif staff: listen interval 3 → up to ~300 ms).
- Flash and RAM numbers: measured here (§6.5); the sketch isn't kept in the repo.
