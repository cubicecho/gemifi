# host-sim: NUC1xx firmware logic on the host

These tests build the real firmware sources with the host `gcc` (as `.so` files, or a plain executable for `midi_test`). Python drives them through `ctypes`, using the independent protocol module [`tools/host/gemifi_proto.py`](../../../../tools/host/gemifi_proto.py) to build requests and check replies.

```sh
make test     # builds into build/ (gitignored) and runs everything; exit status 0 = all passed
make clean
```

You need the host `gcc`, `python3`, and `arm-none-eabi-gcc`. The ARM compiler is there because `make test` first builds `firmware/nuvoton-nuc1xx/hello-led`: its `.gfw` (and a larger 7-page image derived from it) is what the upload tests write into simulated flash.

## What is simulated

| Test | Code under test | Stubbed |
|---|---|---|
| `run_sim.py` (`build/libboot.so`, `build/libboot_test.so`) | `bootloader/nuvoton-nuc1xx/main.c` (normal and `TEST_IN_APROM` builds), `gsx.c`, `crc32.c` | flash (a 128 KB array with real erase/AND-program semantics), SRAM mailbox, buttons, clocks, the SysTick counter, USB (frames go straight to the request handler; replies are captured), `sys_reset_to()` (returns which way it would have reset) |
| `run_app.py` (`build/libapp.so`) | `firmware/nuvoton-nuc1xx/usb-log/main.c` request handling and button messages, `gsx.c` | the same stubs, plus the UID read |
| `midi_test` | `usb_midi.c` SysEx → USB-MIDI packing (CIN 4/5/6/7) and unpacking, 64-byte packets | the USB endpoint layer (`usbd_*`) |

What the checks cover:

- **Uploads.** In 64-, 128- and 256-byte chunks, with page 0 last, including a 7-page image. FW_END validates the image.
- **Error statuses.** Every status the bootloader can return: BAD_CHECKSUM (status-only reply), BAD_CMD, BAD_LEN (including frames too long for the receive buffer), BAD_STATE, BAD_ADDR, BAD_BOARD and BAD_IMAGE (tampered data, fw_version ≠ header).
- **Retries.** A same-`seq` resend gets the cached reply without reprogramming. A resend after BAD_CHECKSUM is executed.
- **Ignored frames.** Reply-range frames and the Identity Request are ignored by the bootloader.
- **Address safety.** No erase or program ever lands outside the target region (APROM, or data flash in the test build). The test build never touches APROM.
- **Boot decision.** A valid app is handed off. These cases stay in update mode: the Pitch▼+Pitch▲ combo, the mailbox `'BOOT'` magic (which is then cleared), a bad CRC, and an even reset vector.
- **usb-log.** HELLO (role 0), the Identity Reply, FW_* → BAD_STATE, REBOOT(1), and the note plus EVENT plus LOG sent on a button press.

## What it can't tell you

- **Anything about the real peripherals.** These are not simulated: the USBD controller (enumeration, EP0 state machine, data toggles, `usbd.c` as a whole), clocks/PLL, GPIO pin mapping and LED polarity, FMC timing and ISPFF behaviour, the UID command on this die, and the CPU_RST/ISPCON.BS handoff.
- **Target compiler behaviour.** Code size, LTO and Thumb code generation are different on the host.
- **Timing.** Host timeouts, flash erase stalls and the polled millisecond counter's drift aren't modelled.

Those need the board: see milestones M1–M4 in `research/core/bootloader-design.md` §10.
