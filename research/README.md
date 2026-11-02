# Research

| Folder | Contents |
|---|---|
| `core/` | Cross-device design: [`framework-research.md`](core/framework-research.md) (overall gemifi architecture, ESPHome/prior art) and [`bootloader-design.md`](core/bootloader-design.md) (bootloader and USB options, browser APIs, protocol rationale, milestone plan) |
| `chips/` | Per chip family: [`nuvoton-nuc1xx/`](chips/nuvoton-nuc1xx/README.md) |
| `devices/` | Per device: [`m-audio-keystation-mini32-mk3/`](devices/m-audio-keystation-mini32-mk3/NOTES.md), [`m-audio-code61/`](devices/m-audio-code61/README.md), [`behringer-fcb1010/`](devices/behringer-fcb1010/fcb1010-brainswap.md) |
| `protocols/` | Host-side protocols: [`host-feedback.md`](protocols/host-feedback.md) (DAW → device feedback: MCU/HUI, MIDI-CI, Ableton, Bitwig, REAPER; plan for #10) |

Each device folder keeps its own notes, analysis, captures and photos. `backup/`, `firmware/` and `vendor/` folders hold stock firmware and vendor files; they stay local and are gitignored.
