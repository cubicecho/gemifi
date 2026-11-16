# switch_matrix

A hub type ([hub](../hub/README.md)): a matrix of up to 32 switches on row and column pins, scanned from the scan interrupt, for a panel's buttons and encoders.

```yaml
switch_matrix:
  - id: grid
    rows: [GPIO4, GPIO5]
    columns:
      - { number: GPIO15, mode: INPUT_PULLUP, inverted: true }
      - { number: GPIO16, mode: INPUT_PULLUP, inverted: true }
buttons:
  - { id: pad_1, pin: { switch_matrix: grid, number: 0 }, action: note, note: 36 }
```

<!-- keys -->

Hub types and the pins they give: [hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components).

Pin n = row n / columns, column n % columns, 1 = closed. Buttons and encoders. It is scanned right after the key matrix, so the two can share column pins (`allow_other_uses: true` on each use); see [Matrices behind decoders](../../core/README.md#matrices-behind-decoders).

## Files

- `switch_matrix.c`: the driver: its rows and columns at init.
- `__init__.py`: the YAML: validation and generated C.
- In the core: its config (`.sm`, `include/gm_device.h`) and state (`gm_hub.h` `u.sm`), with the scan's path in `core/src/vpin.c`: `vpin_scan_matrices` (one load per row where the columns are adjacent bits of one input register), the hardware scan's `vpin_scan_plan` / `vpin_scan_feed`, and the button spans (`vpin_span_take`, `vpin_read_span`).
- Its host test is `core/test/test_matrix.c` (`test_matrix`, `test_matrix_hw`), with the key matrix's.
