# Changelog

Every gemifi release, newest first. Generated from the commit log by semantic-release: `feat:` commits are Features, `fix:` commits are Bug Fixes.

# [0.3.0](https://github.com/cubicecho/gemifi/compare/v0.2.0...v0.3.0) (2026-11-03)


### Features

* **mcu:** finish the Mackie Control protocol: handshake, display and meter hooks, ring centre LED ([cbecf1b](https://github.com/cubicecho/gemifi/commit/cbecf1bf50b84f1cc552e6e6be47ed3be7817bcd)), closes [#18](https://github.com/cubicecho/gemifi/issues/18)

# [0.2.0](https://github.com/cubicecho/gemifi/compare/v0.1.0...v0.2.0) (2026-11-03)


### Bug Fixes

* **stm32f1:** build again after presets; per-slot config store ([8e99313](https://github.com/cubicecho/gemifi/commit/8e99313e9a6a5622039d9c23173ddf49744b9352)), closes [#9](https://github.com/cubicecho/gemifi/issues/9) [#10](https://github.com/cubicecho/gemifi/issues/10)


### Features

* device reports its node name in HELLO; updater matches it to a YAML ([fb8e667](https://github.com/cubicecho/gemifi/commit/fb8e66765e30a086a4786135395055826eb45569)), closes [#4](https://github.com/cubicecho/gemifi/issues/4)
* presets (phase 1): per-preset settings, storage, protocol, host tools ([be0bc48](https://github.com/cubicecho/gemifi/commit/be0bc48d3e3883d3d6de92f4c8eabdcba27e4d51))
* **stm32f1:** STM32F1 platform and M-Audio Code 61 support (untested on hardware) ([f12259f](https://github.com/cubicecho/gemifi/commit/f12259fc983a59a54e6e1c96f9e0f260855fd090))

## 0.1.0 (2026-11-02)

The first tagged version: everything up to the public repo.

### Features

* Device YAML (ESPHome style) → generated C tables → firmware image, with a builder dashboard in Docker
* Web updater: device info, config, debug log and firmware upload over Web MIDI or Web Serial
* Nuvoton NUC1xx platform with a SysEx bootloader; Keystation Mini 32 MK3 support
* ESP32 / ESP8266 platform (USB-MIDI, serial MIDI)
* Host feedback: LEDs driven by the DAW, LED ownership, a DAW port, Mackie Control
