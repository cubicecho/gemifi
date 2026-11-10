# gemifi bootloaders

A gemifi bootloader starts the app firmware, and takes new firmware over USB-MIDI SysEx when asked. The protocol it speaks is in [`../protocol/sysex.md`](../protocol/sysex.md). The design rationale is in [`../research/core/bootloader-design.md`](../research/core/bootloader-design.md) (§3).

| Chip family | Directory | Where it lives | Size budget |
|---|---|---|---|
| Nuvoton NUC100/NUC120 | [`nuvoton-nuc1xx/`](nuvoton-nuc1xx/) | 4 KB LDROM | 4096 B (currently 4084 B) |
| STM32F1 (M-Audio Code 61; any board profile: `make BOARD=generic-stm32f103c8 BOARD_ID=0x3F03`) | [`stm32f1/`](stm32f1/) | 0x08000000, below the app at 0x08002400 | the profile's `boot_size`, 9216 B (currently 6588 B) |

Every gemifi bootloader follows these rules:

- **One protocol.** USB-MIDI, class compliant, so no drivers are needed. Requests are `HELLO`, `REBOOT` and `FW_BEGIN/DATA/END/ABORT` (sysex.md §4).
- **Entry.** The bootloader stays in update mode if any of these is true:
  - the app asked for it (NUC1xx: a `'BOOT'` magic word in the SRAM mailbox; STM32F1: the update-flag page at 0x0801FC00);
  - the board's button combo is held at reset;
  - the app keeps failing: it came back to the bootloader 3 times in a row without running for 5 s (boot-loop guard, `protocol/sysex.md` §6). The count lives in an SRAM mailbox, so a power cycle clears it. On the STM32F1 it counts gemifi images only (M-Audio's app never clears it), and every reset passes through the bootloader there, watchdog resets included;
  - the app image is invalid (header, vectors or CRC).
- **Never writes itself or the chip configuration.** Writes are limited in software to the app region (on STM32F1, plus the update-flag page). On NUC1xx the hardware refuses the rest too: the code never sets LDUEN or CFGUEN.
- **Reset flags for the app.** The bootloader clears the chip's reset flags and leaves a copy in the mailbox, so the app can report why it started (`diag.reset_reason`, `protocol/sysex.md` §4.10).
- **No unlock here.** On a device that opts in to the app's physical unlock (`protocol/sysex.md` §4.9), that guards the way in: REBOOT(1) needs it, and the boot combo and the boot-loop guard need someone at the device or an app that keeps failing.
- **Power-loss safe.** Page 0 (vectors and header) is erased first and written last, and the CRC is checked before every handoff. An interrupted upload therefore always leaves the board in the bootloader.

Shared chip code (startup, clocks, the USB device core, FMC, CRC, the SysEx codec, board files) is in [`../platform/`](../platform/).
