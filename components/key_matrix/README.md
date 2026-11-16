# key_matrix

A control ([core/README.md "Controls"](../../core/README.md#controls)): the keyboard, a matrix of switch keys or velocity keys with two contacts each. At most one per device.

```yaml
key_matrix:
  type: velocity
  rows: [PA8, PA9, PC0, PC1, PC2, PC3, PC6, PC7]
  columns: [PB0, PB1, PB2, PB3, PB4, PB5, PB8, PB12]
  keys: 32
  first_note: 48
  settle: 1us
```

Its YAML (`key_matrix:`): the fields in [core/README.md "Controls"](../../core/README.md#controls), "Key matrix" and "Key matrix options" below, and in core/README.md the scans ([Software scan](../../core/README.md#software-scan), [Hardware scan](../../core/README.md#hardware-scan)) and [Matrices behind decoders](../../core/README.md#matrices-behind-decoders).

## Key matrix

One row is driven at a time (to the active level: high, or low with `inverted`), the columns are read, then the row is released. `type: switch`: key = row + rows × column. `type: velocity`: two contacts per key; key k is on row k % rows and columns 2p, 2p+1 with p = k / rows. Whichever contact closes first starts the timer, the second sends Note On with a velocity from the time between them, and the key is released when both are open. Put diodes on the matrix if more than one key can be held.

## Key matrix options

`type: velocity` keys have two contacts. By default they are columns 2p and 2p+1 of one row (`contacts: columns`, the Keystation). `contacts: rows` puts them on rows 2p and 2p+1 of one column instead (the Code 61). `key_map` says which key sits where when the wiring doesn't follow the default numbering: one list per row (per row pair with `contacts: rows`), one key number per column (per column pair with `contacts: columns`), `null` where there is no key. Without it, key k is row k % rows, column (pair) k / rows. Rows can be MCU pins, sn74hc595 pins or decoder outputs; columns MCU pins or sn74hc165 pins.

```yaml
key_matrix:
  type: velocity
  contacts: rows
  rows: { decoder: rows, first: 0, count: 16 }
  columns: [ ... PB8-PB15 ... ]
  keys: 61
  first_note: 36
  key_map:
    - [55, 54, 53, 52, 51, 50, 49, 48]      # rows 0/1
    # ...
    - [null, null, null, 60, 59, 58, 57, 56]  # rows 14/15
```

## Files

- `key_matrix.c`: scanning the matrix, timing a velocity key's contacts, the key queue, and playing the keys. Built when the device has a key matrix.
- `__init__.py`: the builder part: `validate` (its YAML: rows, columns, contacts, the key map, velocity), `c_defines` (the scan tables' sizes), `c_param_defaults` (the key times) and `c_tables` (`gm_matrix_t`), and when it's built.

## How it runs

The matrix is scanned in the scan interrupt, and its notes are played from the main loop:
- **The software scan.** `gm_scan_tick` calls `matrix_scan` on every tick. It drives one row at a time, reads the columns, and hands each row's reading to `matrix_row`. `matrix_init` works out the fast paths. Rows on MCU pins with an output register take one store per edge, and decoder rows one store per select (`vpin_select_word`). Columns on MCU pins with an input register are read as a few runs of adjacent port bits (`col_run`). The commonest case, every row on an MCU pin with an output register, every column on one input register, no shift-register hubs and a key's contacts on columns (the Keystation), gets a loop of its own (`matrix_scan_fast`, picked by `matrix_init`). The pins see the same thing: row on, `settle`, read, row off. A row that reads as it did on the last tick (nearly always) is then done; only a row that changed has its columns sorted out of the port bits and its keys looked at. On the NUC120 that is about 38 cycles per row besides the settle, down from about 200, so the Keystation's 8 rows take about 21 us of each 125 us tick instead of about 48 (the `scan load` the firmware logs 2.5 s after boot). A row is released sooner after its read than before, and the next one driven sooner, so a column has about 1 us less to fall back between rows: if a board shows ghost notes on the next row, raise `settle`.
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

In `core/test/`: `test_controller.c` (the Keystation: velocity, octave, the fast scan, the keys of its Edit mode's `keys:`, and again with `GM_TEST_SLOW_COLS=1` for the column runs and `GM_TEST_SLOW_ROWS=1` for rows by pin writes) and `test_matrix.c` (`matrix-test.yaml`: decoder rows, row-pair contacts, the key map; built again against `matrix-hw-test.yaml` for the hardware scan).
