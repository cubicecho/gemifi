# Changelog

Every gemifi release, newest first. Generated from the commit log by semantic-release: `feat:` commits are Features, `fix:` commits are Bug Fixes.

## [0.41.1](https://github.com/cubicecho/gemifi/compare/v0.41.0...v0.41.1) (2026-11-10)


### Bug Fixes

* **vpin:** decoder fast tables are filled again on every vpin_init ([d208430](https://github.com/cubicecho/gemifi/commit/d208430347ba3be16aa83373a6c7b963f8b4cb27))


### Performance Improvements

* **vpin:** the mux and decoder selects take the hub index ([#118](https://github.com/cubicecho/gemifi/issues/118)) ([b3afeb3](https://github.com/cubicecho/gemifi/commit/b3afeb3c8dd5b32a261893df7e272c3dbbac199a))

# [0.41.0](https://github.com/cubicecho/gemifi/compare/v0.40.0...v0.41.0) (2026-11-10)


### Bug Fixes

* **builder:** one source of board-reserved pins per board ([a18e6f3](https://github.com/cubicecho/gemifi/commit/a18e6f37828dac843033a3a16475c86504adc9bb))
* **firmware:** read GM_BOARD_PROFILE without its comment ([#116](https://github.com/cubicecho/gemifi/issues/116)) ([549236d](https://github.com/cubicecho/gemifi/commit/549236dc39c7a62964589e5f8bc0835098df1754))


### Features

* **builder:** board: picks the board profile; a generic STM32F103C8 profile ([#116](https://github.com/cubicecho/gemifi/issues/116)) ([12f2637](https://github.com/cubicecho/gemifi/commit/12f26372712081da113e3710026edeccb9506bbe))
* **devices:** diy-bluepill, a Blue Pill DIY example ([#116](https://github.com/cubicecho/gemifi/issues/116)) ([bc9963a](https://github.com/cubicecho/gemifi/commit/bc9963a9a23c35c614d314249b0f83526c00bd5b))
* **mkimage:** DIY board ids with the layout from the build ([#116](https://github.com/cubicecho/gemifi/issues/116)) ([db40a81](https://github.com/cubicecho/gemifi/commit/db40a817e5144e0ec47e08e8adfafce767f49416))
* **nuc1xx:** board profile for the Keystation Mini 32 ([#116](https://github.com/cubicecho/gemifi/issues/116)) ([4d4a01f](https://github.com/cubicecho/gemifi/commit/4d4a01f951152d5de1b57664deb354c6f52e29f5))
* **stm32f1:** board profiles: the layout and board pins as data ([#116](https://github.com/cubicecho/gemifi/issues/116)) ([2f4b1a1](https://github.com/cubicecho/gemifi/commit/2f4b1a1643fa6bfe7f0cd6434485f4cbbe3173c6))
* **web:** flash a DIY board's .gfw with its .board.json ([#116](https://github.com/cubicecho/gemifi/issues/116)) ([17092e1](https://github.com/cubicecho/gemifi/commit/17092e18af1a8466798800b2eac05bab24edcaca))

# [0.40.0](https://github.com/cubicecho/gemifi/compare/v0.39.0...v0.40.0) (2026-11-10)


### Features

* **builder:** require a key_matrix only for keys: and sub-modes ([#117](https://github.com/cubicecho/gemifi/issues/117)) ([8393914](https://github.com/cubicecho/gemifi/commit/8393914751f133a76ff0fb9433a5de0071af6aba))
* **core:** modes without a key matrix ([#117](https://github.com/cubicecho/gemifi/issues/117)) ([13b159e](https://github.com/cubicecho/gemifi/commit/13b159e72c5b63b87a72b68984940bf2e6f496c8)), closes [#error](https://github.com/cubicecho/gemifi/issues/error)

# [0.39.0](https://github.com/cubicecho/gemifi/compare/v0.38.3...v0.39.0) (2026-11-10)


### Bug Fixes

* **builder:** key_matrix takes at most 32 rows and 32 columns ([42147b4](https://github.com/cubicecho/gemifi/commit/42147b46cfe99ce05be3694573137f6971b8fd4d)), closes [#115](https://github.com/cubicecho/gemifi/issues/115)
* **builder:** refuse I2C hubs on platforms without an I2C driver ([e4c3106](https://github.com/cubicecho/gemifi/commit/e4c31069f67d8437e4372f02a67061a78f60821c)), closes [#115](https://github.com/cubicecho/gemifi/issues/115)
* **builder:** stop two cascades of follow-on errors ([e76e889](https://github.com/cubicecho/gemifi/commit/e76e8897a9a6c447c033858846cac4ac8efb8354)), closes [#115](https://github.com/cubicecho/gemifi/issues/115)
* **core:** compare LED indices unsigned, so 128+ LEDs light ([1190c7d](https://github.com/cubicecho/gemifi/commit/1190c7d8049df2d366217c09c5aa61b660fd6794)), closes [#115](https://github.com/cubicecho/gemifi/issues/115)


### Features

* **builder:** check the pins the board itself uses ([5f3a67f](https://github.com/cubicecho/gemifi/commit/5f3a67f5a731a1cc3e7d6a81b0003071c1531aea)), closes [#116](https://github.com/cubicecho/gemifi/issues/116) [#115](https://github.com/cubicecho/gemifi/issues/115)

## [0.38.3](https://github.com/cubicecho/gemifi/compare/v0.38.2...v0.38.3) (2026-11-10)


### Bug Fixes

* **builder:** reject a second rgb_serial hub ([8c91419](https://github.com/cubicecho/gemifi/commit/8c914195dedc89c0049afbe4e42febc2b1a02322))
* **fade:** send a chip's brightness with the round it comes from ([4cf9cbc](https://github.com/cubicecho/gemifi/commit/4cf9cbc8c06c25ef47d66eff4b90f469c9c1d422))


### Performance Improvements

* **leds:** pack the per-LED host state (268 B on the Code 61) ([acc1a20](https://github.com/cubicecho/gemifi/commit/acc1a2062304b33e7a7a98e323a96d29b80c00c0))
* **leds:** skip per-LED work no LED needs ([629a497](https://github.com/cubicecho/gemifi/commit/629a497ffe4b2ebd509b2504e79ffa19457a56b9))
* **nuc:** index the config store's newest record per slot ([cc335a0](https://github.com/cubicecho/gemifi/commit/cc335a0e1aa0e9acd6396305ea0eba12984fc3ea))
* **params:** resume GET_SCHEMA windows where the last one stopped ([dda8954](https://github.com/cubicecho/gemifi/commit/dda8954d29cab41499032e50d013b4691ec0251f))
* **presets:** size rec[] (and presets_move's held[]) for one scope ([6cdb43c](https://github.com/cubicecho/gemifi/commit/6cdb43cee71d0deb08631bd0dc2359e3c22a4609))
* **stm32f1:** size the hardware scans' rings for the device's plan ([409e752](https://github.com/cubicecho/gemifi/commit/409e7521165f6f9f254b2245fb6e2de729812944))
* **tm1628:** 4 bus bytes a pass, not 9 ([ba17dd1](https://github.com/cubicecho/gemifi/commit/ba17dd1f828adb5b997ff049710f08d919163cc9))
* **usb:** drain USB OUT at every hal_midi_poll, not once a pass ([ab1a263](https://github.com/cubicecho/gemifi/commit/ab1a263d3042c089454e682f9688ebbbacd5dc83))

## [0.38.2](https://github.com/cubicecho/gemifi/compare/v0.38.1...v0.38.2) (2026-11-10)


### Bug Fixes

* **buttons:** judge a button sample by every scan since the last (0.2.87) ([8b3616a](https://github.com/cubicecho/gemifi/commit/8b3616a0aae34bb57f68214c87b65890e3300e07))
* **buttons:** release after 16 ms open, not 10 (0.2.86) ([51d3c71](https://github.com/cubicecho/gemifi/commit/51d3c717f9d972f5b26506ec558cee48118e7b34))
* **code61:** matrix settle 3 us for the panel buttons (0.2.85) ([b1956d0](https://github.com/cubicecho/gemifi/commit/b1956d01c1021b90bed2c9aa6a3d95276864a8b6))

## [0.38.1](https://github.com/cubicecho/gemifi/compare/v0.38.0...v0.38.1) (2026-11-10)


### Bug Fixes

* **core:** review pass: stuck notes, start-up bend, settings, DIN order, velocity ([d753a28](https://github.com/cubicecho/gemifi/commit/d753a28daa093acceaa6e99d740b221783a7fb24))
* **leds:** fade: a host's black is brightness 0, not the idle look ([33e5346](https://github.com/cubicecho/gemifi/commit/33e5346f3193323ca665181d0c5177f289605a10))

# [0.38.0](https://github.com/cubicecho/gemifi/compare/v0.37.1...v0.38.0) (2026-11-10)


### Bug Fixes

* **leds:** Code 61 pad LEDs dead after a cold start ([6906eb5](https://github.com/cubicecho/gemifi/commit/6906eb5cbb5143e6cda447ba42d58aa2ac0ffc3b))
* **leds:** fade: host colours set the chip's brightness, not idle looks ([400dafc](https://github.com/cubicecho/gemifi/commit/400dafc6640538fa3b041806bba20a3373f298bf))
* **leds:** fade: no jump to full while a host colour is being drawn ([46454b7](https://github.com/cubicecho/gemifi/commit/46454b77d13069c6846792a00505b94ae1eec3b5))
* **pads:** ignore soft spikes; cycle LEDs report owner local ([a79381f](https://github.com/cubicecho/gemifi/commit/a79381f5b09225c7edc843518ce45b7efc5e8711))


### Features

* **leds:** full-colour rgb_serial LEDs, with gamma and white balance ([c732bfa](https://github.com/cubicecho/gemifi/commit/c732bfa3e51b18de10528ed1f33f92047b692199))
* **leds:** rgb_serial rx_pin: resend only what the board didn't answer ([0ac22a7](https://github.com/cubicecho/gemifi/commit/0ac22a75923007ee58f817f153dc50d3d9470b62))
* **leds:** tm1628 fade: chip brightness follows host colours; Code 61 pads idle dim blue ([b3e84f6](https://github.com/cubicecho/gemifi/commit/b3e84f64ecfff16a4196a4cea6285c2924a370cb))
* **web:** demo Breathe in a picked colour; the staggered one is Waves ([76bbd73](https://github.com/cubicecho/gemifi/commit/76bbd73c6806e801f629d02415a96e286b39cfef))
* **web:** feedback demo in full colour ([7d59b7a](https://github.com/cubicecho/gemifi/commit/7d59b7a01930932edbcda6cebd231a8d46cd0562))

## [0.37.1](https://github.com/cubicecho/gemifi/compare/v0.37.0...v0.37.1) (2026-11-09)


### Bug Fixes

* **web:** reinstall cubeui segmented for the 40px strip, and drop the touchTarget workaround ([05ee278](https://github.com/cubicecho/gemifi/commit/05ee278ed1125777745c47dd66366df427ea85b6)), closes [cubeui#540](https://github.com/cubeui/issues/540) [cubeui#541](https://github.com/cubeui/issues/541)

# [0.37.0](https://github.com/cubicecho/gemifi/compare/v0.36.0...v0.37.0) (2026-11-06)


### Features

* **bootloader:** STM32F1 boot-loop guard, reset flags for the app ([90b4d9c](https://github.com/cubicecho/gemifi/commit/90b4d9c6ab97f1eb03d2d2b7863cdfee5fc19856))
* **core:** physical unlock is opt-in ([b76b013](https://github.com/cubicecho/gemifi/commit/b76b013c98e08a3e008b65b2189767c06ab7e9d2))
* **core:** physical unlock, factory-reset combo, status LED, diagnostics ([5b9b16f](https://github.com/cubicecho/gemifi/commit/5b9b16f8449f8e9b0a6bcfecfa1ca96f6734deb8))
* **host:** wait for the physical unlock, unlock and hello --diag commands ([d1ed4b4](https://github.com/cubicecho/gemifi/commit/d1ed4b4d7e0cc490c9a089406ec655913498dae2))
* **web:** unlock dialog and Diagnostics in the updater ([3be3f46](https://github.com/cubicecho/gemifi/commit/3be3f4657f8a37f2370596c332ab1cd3afecc1ea))

# [0.36.0](https://github.com/cubicecho/gemifi/compare/v0.35.0...v0.36.0) (2026-11-06)


### Features

* **builder:** layout: in the device YAML schema; unknown layout ids warn ([7f08171](https://github.com/cubicecho/gemifi/commit/7f08171259e167cd03f5f7a1d447c61742e82819))
* **core:** device layout from the YAML, and the raw readings stream ([11d28c5](https://github.com/cubicecho/gemifi/commit/11d28c597a74f1600edafb9ff57a78a58979cf71))
* **host:** gemifi.py layout and monitor ([d6a9c63](https://github.com/cubicecho/gemifi/commit/d6a9c63403480b4936089cd483f8e4f1846c01f7))
* **protocol:** GET_LAYOUT, MONITOR and RAW for drawing and testing a device ([9dd56d9](https://github.com/cubicecho/gemifi/commit/9dd56d94df26983812832fe486f24e66bf9aa490))
* **web:** device drawing, Tester view and MIDI learn ([e10b987](https://github.com/cubicecho/gemifi/commit/e10b987df71a15cd66fff4e85529ed950ca78529))

# [0.35.0](https://github.com/cubicecho/gemifi/compare/v0.34.0...v0.35.0) (2026-11-06)


### Features

* **builder:** MIDI message keys in the device YAML schema ([94f1da3](https://github.com/cubicecho/gemifi/commit/94f1da3b115911adaff3849a98b114a6ea589c30))
* **core:** 14-bit CC, NRPN/RPN, bank select, panic, MMC and SysEx ([71d9d4a](https://github.com/cubicecho/gemifi/commit/71d9d4a30dc0814617f09ce12b70094ed002b9a3)), closes [#90](https://github.com/cubicecho/gemifi/issues/90)
* **web:** leave out enum options the firmware doesn't have ([1a700f8](https://github.com/cubicecho/gemifi/commit/1a700f8c08da71511fc2c30c7d1ef20baa1eaa3f))

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
