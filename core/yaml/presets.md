# Presets

The preset slots: named sets of what the controls send, which the user switches at runtime with a preset button, a mode's key or the editor.

```yaml
presets:
  slots: 8
  list:
    - name: Default                  # slot 1: the rest of the file
    - name: Drums
      program: 9
      midi: { channel: 10 }
      buttons: { sustain: { action: note, note: 36 } }
      encoders: { encoder_1: { cc: 21, mode: relative_offset } }
```

<!-- keys -->

- **Presets off.** Without `presets:` the device has one slot that only holds its settings, and no preset actions or `preset.*` params; one slot is refused for the same reason ([core/README.md "Presets"](../README.md#presets)).
- **Slot 1 and the others.** Slot 1 is the rest of the file, so it takes only `name:` and `program:`. Every other preset lists what differs from it; a value equal to slot 1's is dropped with a warning. Slots not listed start as copies of slot 1, named "Preset N".
- **What a preset can't set.** The hardware side is the same in every preset: pins, LEDs, names, a control's `channel` / `min` / `max`, gestures, calibrations. A preset that sets one is refused with where to set it. Modes, their toggle buttons and their layer twins are global too ([modes](../../components/modes/README.md)).
- **Storage.** The builder checks that every slot's worst case fits the platform's store, and names the largest `slots` that fits ([core/README.md "Presets"](../README.md#presets), "Storage").
