# Changelog

Every gemifi release, newest first. Generated from the commit log by semantic-release: `feat:` commits are Features, `fix:` commits are Bug Fixes.

# [0.34.0](https://github.com/cubicecho/gemifi/compare/v0.33.1...v0.34.0) (2026-11-06)


### Features

* **builder:** device YAML packages, substitutions, config hash and JSON schema ([0641d1b](https://github.com/cubicecho/gemifi/commit/0641d1b7e7c673d9b0d8f5926ac9d51a157fe593))

## [0.33.1](https://github.com/cubicecho/gemifi/compare/v0.33.0...v0.33.1) (2026-11-05)


### Bug Fixes

* **host:** flash a Code 61 through its gemifi bootloader ([b6b7664](https://github.com/cubicecho/gemifi/commit/b6b7664cd1d2ec15100387cf48031a25fd434615))

# [0.33.0](https://github.com/cubicecho/gemifi/compare/v0.32.1...v0.33.0) (2026-11-05)


### Bug Fixes

* **live:** teal for a clip without a colour, as in Bitwig ([541233d](https://github.com/cubicecho/gemifi/commit/541233d350adf6937e4a488ca43fea2b9b812c17))


### Features

* **bitwig:** clip grid (pads, clip_T_S, scene_N, grid_*) ([b5bf778](https://github.com/cubicecho/gemifi/commit/b5bf77849669355fb5cf4a5b4969207be53f8a58))
* **live:** clip grid (pads, clip_T_S, scene_N, grid_*) ([37d274e](https://github.com/cubicecho/gemifi/commit/37d274eda8d7ab2d6085d4ac38059542392a3d88))

## [0.32.1](https://github.com/cubicecho/gemifi/compare/v0.32.0...v0.32.1) (2026-11-05)


### Bug Fixes

* **web:** list pads (kind 7) in the host-feedback view ([37a30d5](https://github.com/cubicecho/gemifi/commit/37a30d57f8240b78742de0ffa5d35200e7536940))


### Performance Improvements

* **bitwig:** one FB_VALUE per control per tick from flush(), no allocation per refresh ([02892b8](https://github.com/cubicecho/gemifi/commit/02892b885c97d4439478b429a38a26751c6995e4))
* **controller:** encoder scan state for scanned encoders only ([3674d8a](https://github.com/cubicecho/gemifi/commit/3674d8ad6d4167259da30907f61ed1aa2ab4c142))
* **live:** read the song once per refresh, wrap only what is used ([98d560f](https://github.com/cubicecho/gemifi/commit/98d560fd135456b2d21de9775bd55ba51b6c861b))
* **proto:** queue replies instead of flushing them ([2c2bda0](https://github.com/cubicecho/gemifi/commit/2c2bda091623788b50251e7abe11db910bafe111))
* **vpin:** don't read I2C expanders that have no inputs ([7eb498f](https://github.com/cubicecho/gemifi/commit/7eb498f0a748eb8a2a3fba33007cdf1690f1a4ff))

# [0.32.0](https://github.com/cubicecho/gemifi/compare/v0.31.0...v0.32.0) (2026-11-04)


### Features

* **code61:** encoder acceleration on, medium (0.2.84) ([38989bd](https://github.com/cubicecho/gemifi/commit/38989bdf859cc9a3c1966ad38c210dcdcac8410a))
* **encoders:** acceleration curves (encoder_settings / acceleration:) ([44b2544](https://github.com/cubicecho/gemifi/commit/44b2544a1630f409144f7a6722a189a550c30a88))

# [0.31.0](https://github.com/cubicecho/gemifi/compare/v0.30.0...v0.31.0) (2026-11-04)


### Bug Fixes

* **core:** encoder turns no longer lost to a race with the scan ([1537a8d](https://github.com/cubicecho/gemifi/commit/1537a8d4bbfb8a10d065ddcf80b3c02aa1064764))


### Features

* **presets:** write_to_flash YAML option; the device no longer saves the last preset on its own by default ([19a5b72](https://github.com/cubicecho/gemifi/commit/19a5b72175c2b2bdccbb69238c9861ef9b0b5eca))


### Performance Improvements

* **core:** a preset switch reads the slot's record once, not twice ([4cf7ed5](https://github.com/cubicecho/gemifi/commit/4cf7ed504b8ad0af430a4d442347acb46be6481a))
* **core:** a scan loop of its own for the common software matrix ([1afc6a0](https://github.com/cubicecho/gemifi/commit/1afc6a00d961b755a7d0a05edaccc7e87506fce1))
* **core:** compile out firmware upload on platforms whose bootloader takes it ([b3805b0](https://github.com/cubicecho/gemifi/commit/b3805b0192c50c7ae0034ca3d5cdd0d94411a90c))
* **core:** GET_SCHEMA walks the table once per window, only up to its end ([1b737d5](https://github.com/cubicecho/gemifi/commit/1b737d5200c5a63d9f816c4a02762d75f716a18a))
* **core:** host feedback skips the component walk for numbers nobody uses ([fcc1be6](https://github.com/cubicecho/gemifi/commit/fcc1be678f19064f32f90b813af09b023aa029cf))
* **core:** keep key start times in 16 bits ([99a6506](https://github.com/cubicecho/gemifi/commit/99a6506b312b0838dc1823a793c79b9ccc53b9f9))
* **core:** keep the button history in the narrowest type ([8a92ef5](https://github.com/cubicecho/gemifi/commit/8a92ef5cb471496b3b6ca6a52b89ac9fde8b59c5))
* **core:** key velocity without a divide or two of its three logs ([1b15fd4](https://github.com/cubicecho/gemifi/commit/1b15fd469cc89c575a0e284f28e8ec560b9f88c9))
* **core:** pitch-bend dead zone and step worked out once, not per reading ([80c90b7](https://github.com/cubicecho/gemifi/commit/80c90b760aae5acd1117d569ad8ba9beed897b4d))
* **core:** send keys before LED and display work ([a9fccbb](https://github.com/cubicecho/gemifi/commit/a9fccbbaa648b8ada0dd5b266bdfd76318e8d000))
* **core:** size the key-matrix scan tables by the device ([c584cf6](https://github.com/cubicecho/gemifi/commit/c584cf6a499f9642d910dab8c186f8f59da3da86))
* **tm1628:** bit-bang with set/reset stores and cycle-counted phases ([19c3a74](https://github.com/cubicecho/gemifi/commit/19c3a741dc5c4ae2027241ad91a817a2a35f71cf))
* **tm1628:** write changed spans with auto-increment ([d384a7d](https://github.com/cubicecho/gemifi/commit/d384a7db79794090a9efedb4989ddbbadedd635e))

# [0.30.0](https://github.com/cubicecho/gemifi/compare/v0.29.0...v0.30.0) (2026-11-04)


### Bug Fixes

* **din:** the controller's notes first, Note Offs never dropped ([ba20ff5](https://github.com/cubicecho/gemifi/commit/ba20ff53aa323483384cf84574c8c3a2a583f7a7))
* **usb:** don't stall the main loop when the host isn't reading ([0e6a6b7](https://github.com/cubicecho/gemifi/commit/0e6a6b79b228f101bab302ab6b35d5a3b8832932))


### Features

* **din:** coalesce continuous messages on DIN out ([8378ac2](https://github.com/cubicecho/gemifi/commit/8378ac2c138cdaa72b2898be8a43a14676603487))
* **din:** running status on DIN out ([fa256ab](https://github.com/cubicecho/gemifi/commit/fa256abf1c1681249647b64e5f890115afc43b03))
* **hal:** hal_din_tx_queued, how much DIN Out has still to send ([6d2f709](https://github.com/cubicecho/gemifi/commit/6d2f70926c4842902acc83354f4bc155d99591b8))


### Performance Improvements

* **core:** build preset save records in one static buffer ([7854fe5](https://github.com/cubicecho/gemifi/commit/7854fe579540dd9c1f01b6a54498107c66f57a3f))
* **core:** index labels from builder tables in flash, not RAM ([7a3fb5c](https://github.com/cubicecho/gemifi/commit/7a3fb5c39f48403cb3fabf66f229804a75c844f9))
* **stm32f1:** keep the config store index between reads ([a38e59a](https://github.com/cubicecho/gemifi/commit/a38e59a3ab2a75f126184e87e5f4c608d9d3ab9e))
* **stm32f1:** pack the config store index to 3 bytes a slot ([78e2795](https://github.com/cubicecho/gemifi/commit/78e27958fbb29df424999ccc2f966a227d88c057))

# [0.29.0](https://github.com/cubicecho/gemifi/compare/v0.28.0...v0.29.0) (2026-11-04)


### Features

* **code61:** Shift + Encoder mode cycles MIDI / ZYN / DEVICE / PAN (0.2.82) ([ba5f0c9](https://github.com/cubicecho/gemifi/commit/ba5f0c9489ab004f159cb83a4d26271ec4a9c5aa))
* **core:** cycle modes, independent layers, global layer settings ([6757d8c](https://github.com/cubicecho/gemifi/commit/6757d8c4f1fc9a166cbc98ad00f358971a6f5c78))
* **hosts:** Live and Bitwig track live layers; Zynthian device params doc ([d584fc6](https://github.com/cubicecho/gemifi/commit/d584fc61285890fe3805f560df905b8543e74306))

# [0.28.0](https://github.com/cubicecho/gemifi/compare/v0.27.0...v0.28.0) (2026-11-04)


### Bug Fixes

* **code61:** B1 is master mute, B2-B9 mute strips 1-8 ([12b3aea](https://github.com/cubicecho/gemifi/commit/12b3aeaa82f3fbac5892c853d91a986d32c7ca06))


### Features

* **code61:** DAW roles, 4 preset slots and a refreshed header ([5ef36f2](https://github.com/cubicecho/gemifi/commit/5ef36f215fc77bdb870e1a9ef261265b516c4161))
* **code61:** pad grid and pan layer (0.2.81); YAML reference examples ([0960976](https://github.com/cubicecho/gemifi/commit/09609764b0a398635792134ee059dc03d3308185))
* **core:** pad and encoder twins, pads as components, fader touch ([875146f](https://github.com/cubicecho/gemifi/commit/875146f14188f81cd36870c0881bbbbef68b1363))
* **modes:** button layers and toggle-with; Code 61 mute / arm ([b758390](https://github.com/cubicecho/gemifi/commit/b758390f7015c89e23d16d82981ce9994ad0c08a))

# [0.27.0](https://github.com/cubicecho/gemifi/compare/v0.26.0...v0.27.0) (2026-11-04)


### Features

* **presets:** delete presets; the later ones move up ([0f0241f](https://github.com/cubicecho/gemifi/commit/0f0241fcdd1c0889b6797133a14a410799f9294d))
* **presets:** move presets up and down ([fc4c93a](https://github.com/cubicecho/gemifi/commit/fc4c93af4f726d2846e42fd68048d55eb0022365))

# [0.26.0](https://github.com/cubicecho/gemifi/compare/v0.25.0...v0.26.0) (2026-11-04)


### Features

* feedback demo in the web updater; FB_TEXT reaches the display ([a57a9ac](https://github.com/cubicecho/gemifi/commit/a57a9ac1701bbb37d119cc0fe7ef68f3de656521))

# [0.25.0](https://github.com/cubicecho/gemifi/compare/v0.24.0...v0.25.0) (2026-11-04)


### Features

* **web:** Presets tab with a slot list, New preset and a pinned save bar ([f2c6f7c](https://github.com/cubicecho/gemifi/commit/f2c6f7c9438c08514d5ab43dce0c0369e7136e61))

# [0.24.0](https://github.com/cubicecho/gemifi/compare/v0.23.0...v0.24.0) (2026-11-04)


### Bug Fixes

* **leds:** resend rgb_serial LEDs while idle; a pad hit shows active for 80 ms ([2ea9d7d](https://github.com/cubicecho/gemifi/commit/2ea9d7d36ffd512cb409912ce8451410586c2ee4))


### Features

* **din:** usb_merge is the default din_midi mode; drop garbled DIN In bytes ([0a91ebf](https://github.com/cubicecho/gemifi/commit/0a91ebf92615f660fd54a16d11becbd344007811)), closes [#54](https://github.com/cubicecho/gemifi/issues/54)
* **stm32f1:** hold the power button 2 s to switch off ([528043a](https://github.com/cubicecho/gemifi/commit/528043a3c31b1b7655c2dfde632f870d6e012b70))

# [0.23.0](https://github.com/cubicecho/gemifi/compare/v0.22.0...v0.23.0) (2026-11-04)


### Features

* **web:** show presets, DIN MIDI ports and the DMA map on Validate; YAML examples ([fe7f3d0](https://github.com/cubicecho/gemifi/commit/fe7f3d050518440da2a267b6a2b653cb79ff30f9))

# [0.22.0](https://github.com/cubicecho/gemifi/compare/v0.21.0...v0.22.0) (2026-11-04)


### Features

* **stm32f1:** movable hardware scan timers; DMA allocator searches ([e8eed0b](https://github.com/cubicecho/gemifi/commit/e8eed0b7c21bd4db7b3eb91dc249bc37eb5cfbc8))

# [0.21.0](https://github.com/cubicecho/gemifi/compare/v0.20.0...v0.21.0) (2026-11-04)


### Features

* **settings:** 3-page settings log on the Code 61, fewer flash writes ([533f5d2](https://github.com/cubicecho/gemifi/commit/533f5d227e09f910287cfdeccf2d169c96e03132))

# [0.20.0](https://github.com/cubicecho/gemifi/compare/v0.19.0...v0.20.0) (2026-11-04)


### Features

* **stm32f1:** DMA channel allocator; dma: switches for rgb_serial and din_midi ([343d4cd](https://github.com/cubicecho/gemifi/commit/343d4cd7cf03f95cb9318261afc395880d2d03cb))

# [0.19.0](https://github.com/cubicecho/gemifi/compare/v0.18.0...v0.19.0) (2026-11-04)


### Features

* **web:** preset controls in the Config card ([1219227](https://github.com/cubicecho/gemifi/commit/1219227b16db70ff6d1cf22f2825b2506ebc4b05))

# [0.18.0](https://github.com/cubicecho/gemifi/compare/v0.17.0...v0.18.0) (2026-11-04)


### Bug Fixes

* **core:** din.c without string.h, for the NUC build ([ffce6e8](https://github.com/cubicecho/gemifi/commit/ffce6e8eec502b27cc3a3371cbe8cebbd94f14c5))


### Features

* 5-pin DIN MIDI In/Out with thru; Code 61 MIDI jacks ([6f09324](https://github.com/cubicecho/gemifi/commit/6f0932435d2196e633266b74a60ce1554ba70227))

# [0.17.0](https://github.com/cubicecho/gemifi/compare/v0.16.0...v0.17.0) (2026-11-03)


### Features

* hold modes and mode button bindings; Code 61 presets on Up/Down, channel with Channel held ([887cb1c](https://github.com/cubicecho/gemifi/commit/887cb1c11c084da7ac3f758874525e0c5b1667fa))

# [0.16.0](https://github.com/cubicecho/gemifi/compare/v0.15.0...v0.16.0) (2026-11-03)


### Features

* LED idle/active looks; Code 61 LEDs lit by default ([f3f9f5c](https://github.com/cubicecho/gemifi/commit/f3f9f5c78fdd9a1aec50ab449da49aa26e585108))

# [0.15.0](https://github.com/cubicecho/gemifi/compare/v0.14.0...v0.15.0) (2026-11-03)


### Features

* 4-wire resistive XY pads; Code 61 XY pad ([1c49b09](https://github.com/cubicecho/gemifi/commit/1c49b09815616d32af9ecc6ac557bc084a56b792))

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
