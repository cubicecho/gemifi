# platform/stm32f1

Bare-metal gemifi platform for STM32F103-class chips (Cortex-M3, 72 MHz) that run **behind a bootloader** at the start of flash. Each board is a **board profile**, `boards/<board>.json` (core/README.md "Board profiles"), plus a board header for its reset-time code: `make BOARD=<board>` (the builder passes the YAML's `board:`). The build generates `board_layout.h` (the `BOARD_*` macros) and the linker MEMORY include from the profile into `<build>/board/` (`../board-profile.mk`, `tools/builder/gemifi_board.py`); `ld/*.ld`, `gemifi_image.h`, `recovery.h`, `sys.h`, `sys.c` (PLL from `crystal_hz`), `config_store.c` and the bootloader take their values from it. Boards: `m-audio-code61` (the default; vendor layout, board id `0x0002`) and `generic-stm32f103c8` (a Blue Pill DIY board, 64 KB, the update LED on PC13; board id from the device YAML, `make BOARD=generic-stm32f103c8 BOARD_ID=0x3F03`).

The rest of this page is the Code 61 (`boards/m-audio-code61.json` / `.h`). Its chip is a genuine STM32F103RBT6 (128 KB flash, 1 KB pages, 20 KB RAM, 8 MHz HSE; confirmed on hardware 2026-11-03, #24). It runs the gemifi bootloader ([`bootloader/stm32f1`](../../bootloader/stm32f1/)), put on once over SWD. It replaced the vendor's and keeps the same app address, so an app built for one runs under the other. The 60-slot vector table stays, so the header stays at 0x130 for the tools.

Background: [research/devices/m-audio-code61/analysis/custom-firmware-path.md](../../research/devices/m-audio-code61/analysis/custom-firmware-path.md).

Apps: `firmware/stm32f1/probe` (the first hardware test) and `firmware/stm32f1/midi-controller`.

## Memory map

| Range | Use |
|---|---|
| 0x08000000–0x080023FF | the bootloader: the gemifi bootloader (`bootloader/stm32f1`, written once over SWD), or the vendor's on a unit that still has it. The app never writes it |
| 0x08002400–0x0801AFFF | gemifi image (`ld/app.ld`): vectors 0x000–0x12F (16 core + 60 IRQ), `.gfw` header at +0x130 (0x08002530), then code |
| 0x0801B000–0x0801B3FF | vendor "app valid" page. Only the vendor route uses it: its host packer writes the whole-image checksum at 0x0801B3FE (the negated 16-bit byte sum of 0x08002400–0x0801B3FD). The gemifi bootloader ignores it; the app never writes it |
| 0x0801B400–0x0801FBFF | vendor settings "EEPROM". The gemifi profile uses `BOARD_CONFIG_BASE`/`BOARD_CONFIG_PAGES` in here (Code 61: 0x0801EC00, 0x0801F000 and 0x0801F400, the pages the stock app never uses; never 0x0801F800, the vendor's preset names) |
| 0x0801FC00–0x0801FFFF | update-request flag page, read by both bootloaders |
| 0x20000000–0x20004FFF | RAM, taken as 20 KB. The stack starts at the top. `.noinit` is not zeroed |

The linker checks that the image starts at `BOARD_APP_BASE` (0x08002400), that the header sits at +0x130 (0x08002530) and that the image ends below `BOARD_APP_END` (0x0801B000). `mk/app.mk` calls `tools/host/mkimage.py --layout <build>/board/layout.json` (header 0x130, base 0x08002400, max 102398 bytes) and copies the layout next to the `.gfw` as `<name>.board.json`. A 2 KB-page part needs a page-aligned `boot_size` (the profile check refuses 0x2400 there), so no app erase reaches the bootloader. The gemifi bootloader takes the `.gfw` as it is (`protocol/sysex.md` §4.2); for a unit on the vendor bootloader, the host packer turns it into the vendor record stream (`tools/README.md`, "M-Audio Code 61").

## Update requests and recovery

- **REBOOT(1)**, from `hal_reboot(1)`: writes the update flag `55 5A A5 AA 59 95 3A A3` to 0x0801FC00 (the rest of the page stays FF), disconnects USB and resets with SYSRESETREQ. The gemifi bootloader clears the flag and stays in update mode; the vendor bootloader reads the same flag. **REBOOT(0)** is a plain reset.
- **HELLO caps bit 7** (`hal_extra_caps`): set when the gemifi bootloader's mark is in flash, so hosts know REBOOT(1) leads to a gemifi bootloader and send the gemifi upload, not the vendor one.
- **Recovery combo**, in `src/recovery.c`: runs first in `Reset_Handler`, before `.data`/`.bss`, VTOR, clocks or USB. If the board's `BOARD_RECOVERY_*` buttons are held, it writes the same flag and resets. It uses only registers and its own flash routines.
  - **Code 61**: JTAG off (SWD kept), PB3–PB7 driven low (decoder row 16), PB8–PB15 pulled up, recovery when PB8 and PB9 both read low in 8 samples over ~20 ms: **Up + Down** (verified on hardware 2026-11-03). The pins are then put back to input floating. The gemifi bootloader checks the same combo itself, so it works even with a broken app.
- **IWDG**: about 1 s (PR 3, RLR 0x4E2). Apps start it first thing and kick it in the main loop, so a hang becomes a reset.
- **SRAM mailbox and boot-loop guard** (`protocol/sysex.md` §6), with the gemifi bootloader from 0.1.2: 16 bytes at 0x2000_4FF0, above the stack top (`include/sys.h`). The bootloader counts hand-offs of a gemifi image and stays in update mode after the third without a clear (`reason` 2); a POR or low-power reset zeroes the count; M-Audio's app isn't counted. It also leaves `RCC_CSR` there and clears it. The app (`hal_boot_capture`, `src/hal.c`) reads the reset reason and `reason` from it, clears `boot_count` after 5 s and sets `reason` 1 on REBOOT(1). Under M-Audio's bootloader, or a gemifi one before 0.1.2, the app ignores the mailbox and reads `RCC_CSR` itself. Host test: `test/boot-sim` (`make -C platform/stm32f1/test/boot-sim test`).

## Files

| File | Contents |
|---|---|
| `include/stm32f1.h` | registers from RM0008 and the core registers. USB registers are in `stm32f1_usb.h` |
| `src/startup.c` | vector table and `Reset_Handler`: SP, then `recovery_check()`, then VTOR, then `.data`/`.bss` |
| `src/sys.c` | JTAG-off remap (`BOARD_SWJ_NOJTAG`); clocks: HSI, RCC reset, HSE (or HSE/2) × `BOARD_PLL_MUL` = 72 MHz (Code 61: 8 MHz × 9) with 2 wait states and prefetch, APB1 /2, USB /1.5. If HSE fails it falls back to HSI/2×12 = 48 MHz. Also the polled SysTick ms counter, IWDG and reset |
| `src/flash.c` | unlock, 1 KB page erase (verified blank afterwards), halfword/word program |
| `src/hal.c` | `gm_hal.h`. GPIO through CRL/CRH/BSRR. DIN MIDI (`hal_din_*`) is USART1, TX PA9 and RX PA10 at 31250 baud, through 512/256-byte rings (`hal_din_tx_queued` tells the core how much of the TX ring is still to go). By default its interrupt (priority 0x00) fills and empties them. With `HAL_UART_DMA` (`din_midi dma: true`), DMA1 channel 4 sends each run of the TX ring and channel 5 fills the RX ring in circular mode. `rgb_serial` (USART2) is DMA channel 7, or its interrupt with `dma: false`. core/README.md "DMA channels" lists which features use which channel. `hal_pin_out_reg` uses the ODR bit-band alias. `hal_adc_read` uses ADC2 (ch0–15), so it works alongside the hardware analog scan on ADC1 (an XY pad is read that way). `HAL_ANALOG` sets a pin to analog mode. DWT `hal_delay_us`. UID from 0x1FFFF7E8 |
| `src/config_store.c` | per-slot settings records in a log over `BOARD_CONFIG_PAGES` halfword-programmed 1 KB pages (Code 61: 3). A record is appended wherever it fits while another page is a spare (holds no slot's only copy of its newest record); when nothing fits, the page with the fewest live words is copied into the spare, then the new record, so nothing is erased while it holds the only copy of a slot. Room: every slot's record together (L) and the largest (M) need L + M ≤ 2048 B on 3 pages. Records are packed (v3, this platform only): one group per param id, values as int8/int16/int32 with a bitmap of the stored indices, so the Code 61's worst-case preset (153 values) is 304 B and up to 5 presets + the globals fit (3 use 1020 of 1744 B; the builder warns from 5, errors from 6). The two-page layout of earlier firmware and old v2 records still load as they are. Test: `make -C test/cs-test test` (host gcc; power cuts in every program and erase, also a second cut while recovering) |
| `src/scan_dma.c` | the hardware matrix scan (`hardware_scan:`, core/README.md "Hardware scan"). On the Code 61, TIM1 paces DMA1 channels 5 (update: row select to BSRR), 2 and 6 (CC1/CC3: the column CRL/CRH word back to input / to output, for precharge) and 3 (CC2: column IDR to a 16-frame RAM ring, with a half/full interrupt at 0x80 that hands 8 frames to the core; only the column bits are compared). `hardware_scan.matrix.timer` moves it to TIM2..4 or other compares (`include/dma_map.h`, from the builder; the interrupt is the read channel's). With the select on a compare, every step is one tick later. Transfer errors are counted; stopping puts precharged columns back to inputs |
| `src/adc_dma.c` | the hardware analog scan (`hardware_scan.analog`). On the Code 61, TIM4 CC2 → DMA1 channel 4 stores each mux step on the shared select lines' BSRR, CC4 (PWM mode 2, CC4E; PB9 isn't AF so no pin moves) triggers ADC1's regular group in scan mode, DMA1 channel 1 copies the readings to a two-sweep ring whose half/full interrupt (0x80) hands each sweep to the core. `hardware_scan.analog.timer` moves it: TIM3 triggers by TRGO = OC2REF (no output enabled), TIM2 by CC2 (CC2E: PA1 moves only if it is an AF output; on the Code 61 it is an analog input). 41.5-cycle samples; the period leaves 2 us after the conversions, and a check at the end of each sweep restarts both if they ever fall out of step |
| `include/dma_map.h` | the timers and DMA1 requests of the scans and the scan tick (TIM2, or TIM3/TIM4 when a scan has TIM2), from `gm_device_config.h` (builder `allocate_dma`); without a config, the Code 61's original map. core/README.md "DMA channels" |
| `src/usb_midi.c`, `crc32.c`, `mini_libc.c` | copied from the NUC platform. Keep them in sync |
| `src/usbd.c` | USB FS device driver (separate owner) |

## Unverified (needs hardware)

- The vendor route on a unit that still has M-Audio's bootloader: whether it takes a gemifi image (it has taken the stock image back, 2026-11-03), and whether a soft reset with the flag set is enough or it always needs a power cycle.
- `DBGMCU_IDCODE` reads 0 without a debugger on genuine ST F103s (errata). The probe also reports SCB CPUID: r1p1 on ST, r2p1 on GD32.
