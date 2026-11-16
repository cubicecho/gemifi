# decoder

A hub type ([hub](../hub/README.md)): one or more 74HC138-style one-of-N decoders sharing address lines, which select a device's matrix rows from a few MCU pins.

```yaml
decoder:
  - id: rows
    address: [PB3, PB4, PB5]
    groups:
      - { PB6: 1, PB7: 0 }     # outputs 0-7
      - { PB6: 0, PB7: 1 }     # outputs 8-15
      - { PB6: 0, PB7: 0 }     # outputs 16-23
```

<!-- keys -->

Hub types and the pins they give: [hub](../hub/README.md), [core/README.md "Components"](../../core/README.md#components).

Output n = address n % 2^address-pins in group n / 2^address-pins. Its outputs are one-of-N row selects, so only matrix rows (`key_matrix`, `switch_matrix`), given as `rows: { decoder: rows, first: 16, count: 4 }` or `{ decoder: rows, number: n }` pins. An output is selected when its row is driven ("on") and stays selected until the next row's select replaces it ("off" does nothing). See [Matrices behind decoders](../../core/README.md#matrices-behind-decoders).

## Files

- `decoder.c`: the driver: its address and enable lines.
- `__init__.py`: the YAML: validation and generated C.
- In the core: its config (`.dec`, `include/gm_device.h`), with the scan's path in `core/src/vpin.c`: `vpin_write` selects an output itself, with one precomputed set/reset store where its lines are all on one port (`vpin_select_word` gives the key scan that store).
- Its host test is `core/test/test_matrix.c` (rows behind decoders, as on the Code 61).
