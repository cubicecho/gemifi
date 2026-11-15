# key_matrix

A control ([core/README.md "Controls"](../../core/README.md#controls)): the keyboard, a matrix of switch keys or velocity keys with two contacts each. At most one per device. Its YAML (`key_matrix:`) and what each setting does: [core/README.md "Controls"](../../core/README.md#controls) ("Key matrix"), "Device YAML" ("Key matrix options"), "Software scan", "Hardware scan" and "Matrices behind decoders".

- `key_matrix.c`: scanning the matrix, timing a velocity key's contacts, the key queue, and playing the keys. Built when the device has a key matrix.
- `__init__.py`: the builder part: `validate` (its YAML: rows, columns, contacts, the key map, velocity), `c_defines` (the scan tables' sizes), `c_param_defaults` (the key times) and `c_tables` (`gm_matrix_t`), and when it's built.

## How it runs

The matrix is scanned in the scan interrupt, and its notes are played from the main loop:
- **The software scan.** `gm_scan_tick` calls `matrix_scan` on every tick. It drives one row at a time, reads the columns, and hands each row's reading to `matrix_row`. `matrix_init` works out the fast paths. Rows on MCU pins with an output register take one store per edge, and decoder rows one store per select (`vpin_select_word`). Columns on MCU pins with an input register are read as a few runs of adjacent port bits (`col_run`). The commonest case, every column on one input register and no shift-register hubs (the Keystation), gets a loop of its own (`matrix_scan_fast`) that skips a row which reads as last time.
- **The hardware scan.** The platform's timer and DMA scan the rows, and `gm_scan_frames` (`core/src/controller.c`) hands each frame's changed key rows to `matrix_frame`. `matrix_plan` gives the rows' selects and the column register for the platform's plan.
- **Keys.** `matrix_row` turns a row's changes into key events. A velocity key's first contact starts its timer, and the second queues a Note On with the time between them. The queue holds 32 events. A change that doesn't fit is left as it was, so the next scan sees it again (`matrix_retry` for the hardware scan).
- **Notes.** The main loop calls `keys_poll` (three times a pass, so a key never waits long). It turns each event into a note, with the velocity curve (`velocity`), the octave and transpose. While a mode with `keys:` is on, a key runs the mode's binding instead (`keys_mode`, `mode_key`).

A preset switch (`gm_release_all`) and a mode that takes the keys call `keys_off`, which sends Note Off for every sounding key.

## What it shares

Through `core/include/gm_controls.h`:
- the functions above, for the scan, `gm_init`, `gm_poll` and the modes;
- `mprev`, each row's last reading, for the raw readings (`GM_MONITOR`);
- `keys_down`, the keys held, for the idle time (`gm_idle_ms`).

It reads the scan's `scan_ticks` (the time a key's contacts are measured in), and `hw_on` and `hw_frame_q4` for the hardware scan's frame period.

## Tests

In `core/test/`: `test_controller.c` (the Keystation: velocity, octave, the fast scan, and again with `GM_TEST_SLOW_COLS=1` for the column runs), `test_matrix.c` (`matrix-test.yaml`: decoder rows, row-pair contacts, the key map; built again against `matrix-hw-test.yaml` for the hardware scan) and `test_modes.c` (mode keys).
