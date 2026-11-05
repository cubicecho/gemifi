// The chip families gemifi builds for, for the /platforms/ page. Every figure
// here comes from the repo: the linker scripts (platform/*/ld), the bootloader
// READMEs, platform/esp/README.md, core/performance.md and the builder's
// storage budgets (tools/builder/README.md). Hardware status follows the
// test trackers: #7 (Keystation), #24 (Code 61), #25 (ESP). Check a figure
// there before changing it here.
//
// `status` is a key of statuses.js. `devices` are device page slugs.
// Fact values may hold HTML; `blob` links are completed in the template.
export default [
  {
    id: "stm32f1",
    name: "STM32F1",
    keys: ["stm32f1"],
    status: "works",
    summary:
      "STM32F103-class chips (Arm Cortex-M3). The fullest platform: timer and DMA scanning, DIN MIDI and every component in use.",
    facts: [
      ["Checked chip", "STM32F103RBT6 (in the M-Audio Code 61)"],
      ["CPU", "Arm Cortex-M3 at 72 MHz, from an 8 MHz crystal"],
      ["RAM", "20 KB. The Code 61 build uses about 12.8 KB."],
      [
        "Flash",
        "128 KB in 1 KB pages: the gemifi bootloader in the first 9 KB (about 6.3 KB used), the app from 0x08002400 to 0x0801AFFF (99 KB; the Code 61 build is about 60 KB), and the settings in three 1 KB pages",
      ],
      [
        "MIDI",
        "USB-MIDI (class compliant, no driver), with an optional second port for Mackie Control or the DIN jacks; 5-pin DIN MIDI on USART1 (PA9, PA10)",
      ],
      [
        "Install and updates",
        "The gemifi bootloader goes on once over SWD. After that, apps are installed over USB from the browser. It stays in update mode when the app asks for it, when the board's button combo is held at power-on (Code 61: Up + Down), or when there is no valid app. No boot-loop guard yet.",
      ],
      [
        "Scanning",
        "<code>hardware_scan:</code> timers and DMA scan the key and button matrices (36 µs frames on the Code 61) and the faders and pads (ADC1). The builder shares out the 7 DMA channels and says what clashes. Software scan otherwise.",
      ],
      [
        "Settings store",
        "A log over three 1 KB pages with packed records, built so a power cut during a save loses at most that save (tested on the host, not yet on hardware). The Code 61's 4 preset slots need up to 1460 of 1744 bytes; up to 5 fit.",
      ],
      [
        "On hardware",
        "Checked on the Code 61 on 2026-11-03: bootloader, app updates over USB, keys, buttons, encoders, faders, wheels, aftertouch, XY pad, pads and their LEDs, display, DIN MIDI, presets and settings. Not yet: a power cut during a save, the scans on other timers with DIN by DMA, pad LEDs without DMA, and the newer layers.",
      ],
    ],
    devices: ["m-audio-code"],
    docs: [
      ["platform/stm32f1/README.md", "Platform notes"],
      ["bootloader/README.md", "Bootloaders"],
      ["core/README.md#dma-channels-stm32f1", "DMA channels"],
    ],
    issue: 24,
  },
  {
    id: "nuvoton-nuc1xx",
    name: "Nuvoton NUC1xx",
    keys: ["nuvoton-nuc1xx"],
    status: "works",
    summary:
      "Nuvoton NUC100/NUC120 (Arm Cortex-M0). Small: 4 KB of RAM and 32 KB of app flash, so the core is written to fit it.",
    facts: [
      ["Checked chip", "NUC120LC1BN (in the Keystation Mini 32 MK3), a part Nuvoton has since made obsolete"],
      ["CPU", "Arm Cortex-M0 at 48 MHz (rated 50 MHz), from a 12 MHz crystal. No divide instruction."],
      ["RAM", "4 KB; the top 16 bytes are the bootloader's mailbox. The Keystation build uses about 1.8 KB and keeps at least 512 bytes for the stack."],
      [
        "Flash",
        "32 KB for the app (APROM; the Keystation build is about 25 KB), 4 KB of boot flash (LDROM) for the bootloader (4080 of 4096 bytes used), and 4 KB of data flash for the settings",
      ],
      ["MIDI", "USB-MIDI (class compliant, no driver), with an optional second port for Mackie Control. No DIN MIDI yet."],
      [
        "Install and updates",
        "The gemifi bootloader goes in LDROM once, over SWD, after a verified backup. After that, apps are installed over USB from the browser (about 4 s for the Keystation app). The bootloader never writes itself or the chip configuration, and the chip enforces that. It stays in update mode when the app asks, when the button combo is held (Keystation: Pitch ▼ + Pitch ▲), when the app is invalid, or when the app has come back to it 3 times in a row without running for 5 s (boot-loop guard).",
      ],
      [
        "Scanning",
        "Software, from a timer tick. With rows and columns on MCU ports a fast path skips unchanged rows: the Keystation's 8 rows take about 21 µs of each 125 µs tick. No DMA scan.",
      ],
      ["Settings store", "A journal in data flash: 7 pages of 512 bytes, 5 usable for records. The Keystation's 8 preset slots need up to 1908 of 2560 bytes; up to 9 fit."],
      [
        "On hardware",
        "Checked on the Keystation on 2026-10-31: bootloader, app updates over USB, keys with velocity, buttons, knob, LEDs and settings. Merged but not yet checked: the boot-loop guard and stored serial number, presets and modes, DAW feedback and the DAW port.",
      ],
    ],
    devices: ["keystation-mini32-mk3"],
    docs: [
      ["bootloader/nuvoton-nuc1xx/README.md", "Bootloader"],
      ["firmware/nuvoton-nuc1xx/midi-controller/README.md", "App"],
      ["tools/README.md", "SWD runbook"],
    ],
    issue: 7,
  },
  {
    id: "esp",
    name: "ESP32 and ESP8266",
    keys: ["esp32", "esp32s2", "esp32s3", "esp8266"],
    status: "in-progress",
    summary:
      "Espressif chips on any board PlatformIO knows, on the Arduino framework. No probe needed: the chip's ROM bootloader does the first install.",
    facts: [
      ["Chips", "ESP32, ESP32-S2, ESP32-S3 and ESP8266. The examples use an ESP32-S3-DevKitC-1 and a Wemos D1 mini."],
      ["CPU", "ESP32 and ESP32-S3: Xtensa at 240 MHz. ESP8266: 80 or 160 MHz."],
      ["RAM", "Not measured for gemifi yet"],
      [
        "Flash",
        "ESP32 family: two app slots of 1.9 MB each, plus NVS for the settings. ESP8266 (D1 mini, 4 MB): about 1 MB for the app, and an update must fit the free half.",
      ],
      [
        "MIDI",
        "USB-MIDI on the S2 and S3 (native USB, the default there); serial MIDI through the board's USB-serial chip on all of them (115200 baud, used with Web Serial); BLE MIDI on the ESP32 and S3 (adds about 800 KB of flash on the ESP32; untested)",
      ],
      [
        "Install and updates",
        "No gemifi bootloader. The first install goes through the chip's ROM bootloader, from the browser over Web Serial or with esptool. Later updates go over MIDI into the spare app slot (A/B): the running firmware keeps booting until the new one has been checked, so a failed upload can't brick it.",
      ],
      [
        "Scanning",
        "Software. ESP32: a timer task at a priority above the main loop, at most every 100 µs. ESP8266: from the main loop, with some jitter, and flash writes during an update stall it for 30–50 ms. Pins go through one call each (no fast paths yet, #71); the ESP8266's only ADC pin, A0, takes about 100 µs a reading.",
      ],
      ["Settings store", "ESP32 family: NVS, written atomically, 12 KB for all presets. ESP8266: one 4 KB EEPROM sector; a power cut during a save loses the settings (the defaults load)."],
      ["On hardware", "Nothing yet. The builds compile and pass the host tests."],
    ],
    devices: ["diy-esp32", "behringer-fcb1010"],
    docs: [
      ["platform/esp/README.md", "Platform notes"],
      ["firmware/esp/midi-controller/README.md", "App"],
    ],
    issue: 25,
  },
];
