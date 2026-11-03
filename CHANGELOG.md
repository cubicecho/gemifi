# Changelog

Every gemifi release, newest first. Generated from the commit log by semantic-release: `feat:` commits are Features, `fix:` commits are Bug Fixes.

# [0.14.0](https://github.com/cubicecho/gemifi/compare/v0.13.0...v0.14.0) (2026-11-03)


### Bug Fixes

* **display:** draw '#' without its bottom line; Code 61 0.2.54 ([ef1235e](https://github.com/cubicecho/gemifi/commit/ef1235e7865673517bac1963f7c15407f0b9425b))
* **updater:** Code 61 uploads survive Chrome freezing on Windows ([cbf9575](https://github.com/cubicecho/gemifi/commit/cbf957589fa8d7ba45e52f7c116331949d6f3af4))


### Features

* channel pressure from analog inputs; Code 61 aftertouch ([55087b4](https://github.com/cubicecho/gemifi/commit/55087b49a6ae57f9b0e776144b792f8e31816668))
* pitch bend from analog inputs; Code 61 pitch and mod wheels ([e3575d4](https://github.com/cubicecho/gemifi/commit/e3575d46e8fe40ed45b23c135c17042169f0e80f))


### Performance Improvements

* **stm32f1:** scan the Code 61 by timer + DMA, cheaper LED writes ([b1509da](https://github.com/cubicecho/gemifi/commit/b1509da1dde894e92b50ea25a923b463366b0585))

# [0.13.0](https://github.com/cubicecho/gemifi/compare/v0.12.0...v0.13.0) (2026-11-03)


### Features

* 14-segment displays; the Code 61's shows the last MIDI message ([dfad297](https://github.com/cubicecho/gemifi/commit/dfad2976eefd54119c13b2e063b8b41b32f387fb))
* **core:** LED walk diagnostic; Code 61 shift has no LED ([c599d53](https://github.com/cubicecho/gemifi/commit/c599d53922016fc890e09783b7d3432d344ace24))

# [0.12.0](https://github.com/cubicecho/gemifi/compare/v0.11.0...v0.12.0) (2026-11-03)


### Features

* **code61:** button mode, button LEDs, pad LEDs, display map ([39adf82](https://github.com/cubicecho/gemifi/commit/39adf829919e45165e9a5c5bb97a724fd90ae2c7))
* rgb_serial LED hub, pad LEDs, faster button debounce ([261088a](https://github.com/cubicecho/gemifi/commit/261088a53813f6da33061abb7335cf3caeeba1b3))

# [0.11.0](https://github.com/cubicecho/gemifi/compare/v0.10.0...v0.11.0) (2026-11-03)


### Features

* **builder:** stm32f1 storage budget counts packed v3 records ([074ab3c](https://github.com/cubicecho/gemifi/commit/074ab3cda4ef8d8a82bbbff1660ebfc19e2f8700))
* **stm32f1:** packed v3 config records fit the Code 61's settings ([25061c7](https://github.com/cubicecho/gemifi/commit/25061c794eea0d9a6aee2f550c3b8656df25c799))

# [0.10.0](https://github.com/cubicecho/gemifi/compare/v0.9.0...v0.10.0) (2026-11-03)


### Bug Fixes

* **code61:** pad rows run top to bottom on the muxes ([cedeffb](https://github.com/cubicecho/gemifi/commit/cedeffb4c1a814691c42d08482be6c10b41b73ae))
* **code61:** scan every 400 us, 4 us mux settle ([5d3bd66](https://github.com/cubicecho/gemifi/commit/5d3bd664e79523313cd1a56dc0f892288af75353))
* **stm32f1:** scan guard times the scan with the cycle counter ([0991e79](https://github.com/cubicecho/gemifi/commit/0991e7937d095e900c80e74a91904fdc4c0f2b89))
* **stm32f1:** scan timing counts the scan, not a late start ([31d8709](https://github.com/cubicecho/gemifi/commit/31d8709abe4434c62d439c0fac9acbae7d3cbe3a))
* **vpin:** muxes that share select lines keep their channel cache right ([d78f827](https://github.com/cubicecho/gemifi/commit/d78f827144f1ec8f15302565bfc03107cc38a204))


### Features

* **code61:** declare the two TM1628 LED drivers, add an LED-map build ([f70d47f](https://github.com/cubicecho/gemifi/commit/f70d47f1bc386f7ad118faa434ae78a0fcab790a))
* **code61:** keys, panel buttons and encoders in the device file ([c7a3a38](https://github.com/cubicecho/gemifi/commit/c7a3a38a5256c3aa9a7bca734c3f2b27bf4f40bf))
* **code61:** name b1-b9 (panel slots 20-28) ([8388919](https://github.com/cubicecho/gemifi/commit/83889190612d796af7b7b474025a490e60a318b1))
* **code61:** name the mode, bank and x/y buttons (slots 30-37) ([09afbc4](https://github.com/cubicecho/gemifi/commit/09afbc49b7cad5ca0b949cf3ad3c11bf8394c482))
* **core:** decoder rows, switch matrices, row-pair key contacts and key maps ([2f4be00](https://github.com/cubicecho/gemifi/commit/2f4be0048d47fb80321f546118190b4a7f2d73d5))
* **core:** time each part of the scan; presets buffer counts pads ([6d65565](https://github.com/cubicecho/gemifi/commit/6d65565c228d89fc66580d66b1bd0ea906c24a10))
* **leds:** TM1628-class LED driver hub and RGB LEDs ([02b0481](https://github.com/cubicecho/gemifi/commit/02b04818326afd91e88b1c4e8f8906eadd22a51c))
* **pads:** velocity pads from pressure (FSR) inputs ([b969d34](https://github.com/cubicecho/gemifi/commit/b969d34b35d1a96a6f4f1c422ea05ad1e01d1fd4))
* **platform:** open-drain output pin mode (HAL_OUT_OD) ([834a8d4](https://github.com/cubicecho/gemifi/commit/834a8d4cb82c2a0141796c101c8c43ffb871cc17))
* **stm32f1:** log the slowest main-loop pass ([04daf37](https://github.com/cubicecho/gemifi/commit/04daf3795801c0002ac72b3197202a743c71b41e))
* **stm32f1:** serve USB from its interrupt; SysTick-counted milliseconds ([5db89d1](https://github.com/cubicecho/gemifi/commit/5db89d1bb1629b189f14878453a6c73bf4a3ddcf))


### Performance Improvements

* cheaper mux switches and analog reads ([2fbc80b](https://github.com/cubicecho/gemifi/commit/2fbc80bc12ab1a70b7d914f56dba85fc951ea951))
* **core:** cut the Code 61 scan cost ([67b2389](https://github.com/cubicecho/gemifi/commit/67b23897a51089d21b31c7c2a050216337d9cc25))
* **core:** one-store decoder select, direct encoder reads ([da76c6e](https://github.com/cubicecho/gemifi/commit/da76c6e56d1057b7fe6dbebd5658056833df3990))
* **core:** spread switch-matrix rows over scans; direct key-row select ([92697b2](https://github.com/cubicecho/gemifi/commit/92697b274bd9f2921af9d552a22deaed484d8f78))

# [0.9.0](https://github.com/cubicecho/gemifi/compare/v0.8.0...v0.9.0) (2026-11-03)


### Bug Fixes

* **stm32f1:** bootloader takes the protocol's 512-byte upload order ([309728a](https://github.com/cubicecho/gemifi/commit/309728ab2e5f0d617545c82a317c158212f4ec69))


### Features

* **updater:** settings and host feedback move to a Config tab ([37a232d](https://github.com/cubicecho/gemifi/commit/37a232d6a7542328f1497819a62fdb5ba2bc631a))

# [0.8.0](https://github.com/cubicecho/gemifi/compare/v0.7.1...v0.8.0) (2026-11-03)


### Bug Fixes

* **stm32f1:** Code 61 apps boot behind the bootloader and keep the power on ([338c61a](https://github.com/cubicecho/gemifi/commit/338c61a1ae41f573094854d7adfac8c0af731103))


### Features

* **stm32f1:** gemifi bootloader for the Code 61, updatable from the web app ([29fb394](https://github.com/cubicecho/gemifi/commit/29fb3943d4ce1de62a02392d7f5eaa041176e097))
* **tools:** SWD helpers for the Code 61 over a Bus Pirate ([d1f64f9](https://github.com/cubicecho/gemifi/commit/d1f64f98502250d8266ab133c529a947d1818874))

## [0.7.1](https://github.com/cubicecho/gemifi/compare/v0.7.0...v0.7.1) (2026-11-03)


### Bug Fixes

* **updater:** don't hold the keyboard's MIDI ports open across a power cycle ([dbcd896](https://github.com/cubicecho/gemifi/commit/dbcd8968d2913861d3aa4708948474ea4b678173))

# [0.7.0](https://github.com/cubicecho/gemifi/compare/v0.6.0...v0.7.0) (2026-11-03)


### Features

* **hosts:** Bitwig uses the added GET_COMPONENT fields and BOOTED ([272510d](https://github.com/cubicecho/gemifi/commit/272510de390494a14371c38fe55783f47d35aa61)), closes [#30](https://github.com/cubicecho/gemifi/issues/30)

# [0.6.0](https://github.com/cubicecho/gemifi/compare/v0.5.0...v0.6.0) (2026-11-03)


### Features

* **core): device modes (presets phase 2:** Edit mode keys pick presets ([92d3c96](https://github.com/cubicecho/gemifi/commit/92d3c96545ce6bc180875e7c7ce146ac275b2d75))

# [0.5.0](https://github.com/cubicecho/gemifi/compare/v0.4.0...v0.5.0) (2026-11-03)


### Features

* **hosts:** Bitwig extension for host feedback (F3) ([f404965](https://github.com/cubicecho/gemifi/commit/f4049650bc393658f7b59e6fdbb52c0d9fe158fb))
* **protocol:** GET_COMPONENT takeover/mode/owner and a BOOTED message ([b124ba6](https://github.com/cubicecho/gemifi/commit/b124ba6d78af107cc7e9be881f99e9449214df4c))

# [0.4.0](https://github.com/cubicecho/gemifi/compare/v0.3.0...v0.4.0) (2026-11-03)


### Bug Fixes

* **core:** drop FB_RING with an unknown mode; spec: the host tool's questions ([d3d1192](https://github.com/cubicecho/gemifi/commit/d3d11921aa24bad3367ef5a283b2e39e0ea36050))


### Features

* FB_RING ring-off flag; debug.events is volatile; F3 spec details ([585991a](https://github.com/cubicecho/gemifi/commit/585991a47a6f69e5e87e8d2b8cd2c6b22a8408ea))
* host feedback (F3) in the core and builder ([bf164c8](https://github.com/cubicecho/gemifi/commit/bf164c83a1bdb1aac2dd118ad9a58f9d314e1d3f))
* **host:** host feedback (F3) codec, components and fb commands ([4c11607](https://github.com/cubicecho/gemifi/commit/4c11607c0e0009ca0642cf0d4b38a76608bef675))
* **hosts:** Ableton Live Remote Script for host feedback (F3) ([a84e971](https://github.com/cubicecho/gemifi/commit/a84e9716b267872c93ea56b6e233ab0b00e735f6))
* **updater:** host feedback (F3) codec and test card ([52cb278](https://github.com/cubicecho/gemifi/commit/52cb278ef43026eaa14524631a9901e4cf09de35))

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
