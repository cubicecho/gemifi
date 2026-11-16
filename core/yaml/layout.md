# Layout

Where each control is on the device, so the web page can draw it and light up the control being moved; the device sends it on request (GET_LAYOUT).

```yaml
layout:
  size: [80, 30]
  keys: [0, 12, 76, 14]
  controls:
    volume: { x: 1, y: 1, w: 4, h: 4 }
    fader_1: { x: 7, y: 1, w: 2, h: 10, shape: fader }
    play: [11, 1]
    stop: [11, 4, 3, 2]
```

<!-- keys -->

- **A place** is `[x, y]` (with its shape's size), `[x, y, w, h]` or `{ x, y, w, h, shape }`, in whole units of about 5 mm from the top left, at most 255 each way.
- **Controls** are buttons, analog inputs, encoders, pads and displays, each once, by id; the shapes each kind can be, and their sizes, are in [core/README.md "Layout"](../README.md#layout). One axis of an `xy_pad` with `shape: xy` draws the pad for both.
- **The keys.** With a `key_matrix` the builder places the keys under the controls by itself; `keys:` places them elsewhere.
- **Without `layout:`** the firmware has no layout and pays nothing for it ([core/README.md "Layout"](../README.md#layout), "Cost").
