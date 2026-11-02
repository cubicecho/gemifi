# host-sim: NUC1xx firmware logic on the host

These tests build the real firmware sources with the host `gcc` (as `.so` files, or plain executables for `midi_test` and `cs_test`). Python drives them through `ctypes`, using the independent protocol module [`tools/host/gemifi_proto.py`](../../../../tools/host/gemifi_proto.py) to build requests and check replies.

```sh
make test     # builds into build/ (gitignored) and runs everything; exit status 0 = all passed
make presets-test  # only the presets end-to-end test (host gcc only)
make cs-test  # only the config_store journal test (host gcc only); CS_ARGS=-q is quicker,
              # CS_ARGS=-long runs 20x the writes without power cuts, CS_ARGS=<n> picks a seed
make clean
```

You need the host `gcc`, `python3`, and `arm-none-eabi-gcc`. The ARM compiler is there because `make test` first builds `firmware/nuvoton-nuc1xx/hello-led`: its `.gfw` (and a larger 7-page image derived from it) is what the upload tests write into simulated flash.

## What is simulated

| Test | Code under test | Stubbed |
|---|---|---|
| `run_sim.py` (`build/libboot.so`, `build/libboot_test.so`) | `bootloader/nuvoton-nuc1xx/main.c` (normal and `TEST_IN_APROM` builds), `gsx.c`, `crc32.c` | flash (a 128 KB array with real erase/AND-program semantics), SRAM mailbox, `GCR_RSTSRC`, the UID command, buttons, clocks, the SysTick counter, USB (frames go straight to the request handler; replies are captured), `sys_reset_to()` (returns which way it would have reset) |
| `run_app.py` (`build/libapp.so`) | `firmware/nuvoton-nuc1xx/usb-log/main.c` request handling and button messages, `gsx.c` | the same stubs, plus the UID read |
| `run_presets.py` (`build/libctl.so`) | the midi-controller's core: `core/src/` (`presets.c`, `app_proto.c`, `controller.c`, `params.c`, `settings.c`, `vpin.c`, `midi_in.c`, `mcu.c`, `gsx.c`) with the device config generated from `devices/keystation-mini32-mk3.yaml`, and `config_store.c` + `crc32.c` | the HAL (`gm_hal.h`): time (advanced from Python), pins (buttons and the two-contact key matrix are computed from the simulated state on every read), the ADC, MIDI out (captured), the UID, reboot; `fmc_*` on the 4 KB data flash, which the Python side owns, so a "reboot" (the library is reloaded, RAM starts fresh) keeps it |
| `midi_test` | `usb_midi.c` SysEx → USB-MIDI packing (CIN 4/5/6/7) and unpacking, 64-byte packets | the USB endpoint layer (`usbd_*`) |
| `cs_test` | `config_store.c` (the per-slot journal in data flash), `crc32.c` | `fmc_*` on a 4 KB data-flash array (erase/AND-program, counts every operation and can cut the power at any one of them) |

What the checks cover:

- **Uploads.** In 64-, 128- and 256-byte chunks, with page 0 last, including a 7-page image. FW_END validates the image.
- **Error statuses.** Every status the bootloader can return: BAD_CHECKSUM (status-only reply), BAD_CMD, BAD_LEN (including frames too long for the receive buffer), BAD_STATE, BAD_ADDR, BAD_BOARD and BAD_IMAGE (tampered data, fw_version ≠ header).
- **Retries.** A same-`seq` resend gets the cached reply without reprogramming. A resend after BAD_CHECKSUM is executed.
- **Ignored frames.** Reply-range frames and the Identity Request are ignored by the bootloader.
- **Address safety.** No erase or program ever lands outside the target region (APROM, or data flash in the test build). The test build never touches APROM.
- **Boot decision.** A valid app is handed off. These cases stay in update mode: the Pitch▼+Pitch▲ combo, the mailbox `'BOOT'` magic (which is then cleared), a bad CRC, and an even reset vector.
- **Boot-loop guard.** `GCR_RSTSRC` is simulated (`sim_rstsrc`, cleared when read). 3 hand-offs without the app clearing `boot_count` → the 4th reset stays in update mode with `reason` 2. POR, LVR and BOD reset a random count and reason; pin, watchdog, `SYSRESETREQ` and CPU resets don't. An app that clears the count is always handed off. After a trap the next reset gets fresh tries.
- **USB serial.** With a chip UID it is the UID. With the UID command refused (`sim_uid_fail`, as on the NUC120BN) it comes from a valid `'GDID'` record at 0x1FE00, and both the serial and the HELLO `uid` match the app's. A bad CRC, wrong magic, a flipped ID bit or an erased page give zeros. The record is never written.
- **config_store journal.** Per-slot write/read, newest wins, names (cut to 16 bytes, `""` = none), revision, entry limits (59 named / 61 unnamed, more is BAD_LEN), tombstones (and `erase_slot` of an empty slot writes nothing), `erase`, slot 64, values of −1 (a blank-looking word). The device-ID page is never touched and no word is ever programmed twice.
  - **v1 migration.** An old journal (v1 records, seq near the 32-bit wrap) reads as slots 0 and 1; writes to other slots copy it forward as v1; after slot 0 is written it is copied as a v2 slot-1 record; a slot-1 reset hides it; a 62-entry v1 keeps 61 entries when converted.
  - **Compaction at the budget edge.** Random writes (mostly worst-case sizes, some resets) with 9–10 slots × 252 B, 2 and 5 slots × 508 B (the record limit), 1 × 508 B + 8 × 252 B, and 33 slots × 52 B. Every slot is checked after every write. Past the budget (8 × 508 B) a write fails with FLASH_ERR and every slot keeps its record.
  - **Power cuts.** On every 4th write (`-q`: every 25th) the write is repeated with the power cut at each of its flash operations in turn (the cut erase or program is done partly: no words, the first k words or random words of an erase; random bits of a program). After the cut every slot must read its old record, the written slot its old or new one. Then a retry is cut once more at a random point (same check), and a last retry must succeed with every slot correct. The run continues from one of the cut states, so later writes start from torn records and half-done compactions.
- **Presets, end to end** (`run_presets.py`, sysex.md §4.5). Requests built and replies parsed with `gemifi_proto.py`:
  - HELLO caps bit 5. The schema (host parser): the PRESET flag on exactly the mapping params, `preset.active` 1..8, `preset.dirty` READ_ONLY | VOLATILE. PRESET_INFO for slots 1..8: names Default/Drums/DAW/Preset 4.., the YAML flag on 1..3, count, revision.
  - Select (SET_PARAM 17): the Drums YAML values read back, PRESET_CHANGED, a button press and a key send on channel 10. A per-preset edit sets dirty (PRESET_CHANGED once, `preset.dirty`, PRESET_INFO), a global one doesn't. Switching away or reselecting drops it.
  - COMMIT, then a reboot: the edit and the active slot are back, PRESET_INFO shows stored edits. PRESET_SAVE (save as), PRESET_NAME (inactive and dirty active slots, empty name, a name alone), PRESET_RESET of one slot and of all (globals kept), FACTORY_RESET (journal erased); every state also after a reboot.
  - `preset_next` / `preset_prev` buttons (set by SET_PARAM): PARAM_CHANGED for `preset.active`, wrap 8 → 1, Note Off on the old channel for a held key and a held note button (and silence when they are released later), the Program Change of a slot with `preset.program`.
  - The `preset.active` debounce: no flash write before 10 s (a reboot then gives the old slot), one write after, none when flipping back to the stored slot; unsaved edits stay unsaved.
  - 400 random COMMITs across slots with reboots in between (the journal is compacted well over 100 times): every slot reads back what was committed. No word is ever programmed twice, no erase or program lands outside the journal, the device-ID page is untouched.
  - Errors: bad slots (BAD_PARAM; BAD_VALUE for `preset.active`), long or non-printable names (BAD_VALUE), wrong lengths (BAD_LEN); none of them changes anything.
  - `gemifi.py` backup → FACTORY_RESET → restore against the real firmware: the backup leaves the device as it was (dirty included), the restore brings every slot back.
- **usb-log.** HELLO (role 0), the Identity Reply, FW_* → BAD_STATE, REBOOT(1), and the note plus EVENT plus LOG sent on a button press.

## What it can't tell you

- **Anything about the real peripherals.** These are not simulated: the USBD controller (enumeration, EP0 state machine, data toggles, `usbd.c` as a whole), clocks/PLL, GPIO pin mapping and LED polarity, FMC timing and ISPFF behaviour, the UID command on this die, and the CPU_RST/ISPCON.BS handoff.
- **Target compiler behaviour.** Code size, LTO and Thumb code generation are different on the host.
- **Flash physics.** A partly programmed word that reads back as blank, or bits that read differently on the next read, aren't modelled.
- **Timing.** Host timeouts, flash erase stalls and the polled millisecond counter's drift aren't modelled.

Those need the board: see milestones M1–M4 in `research/core/bootloader-design.md` §10.
