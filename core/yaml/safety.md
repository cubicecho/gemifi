# Safety and storage

The keys for getting a device back when something goes wrong, keeping a web page from flashing or wiping it unasked, and what it may write to its flash on its own.

```yaml
boot_combo: [pitch_down, pitch_up]
factory_reset_combo: { combo: [oct_down, oct_up], hold: 5s }
unlock: { combo: [pitch_down, pitch_up], hold: 2s }
status_led: edit_led
write_to_flash: { last_preset: true, delay: 10s, idle: 3s }
```

<!-- keys -->

- **Off unless set.** Each key is left out of the firmware when it isn't used. `unlock:` stays off even on a device with a `boot_combo`, and the shipped YAMLs leave it off ([core/README.md "Physical unlock"](../README.md#physical-unlock)).
- **Combos.** The factory-reset combo counts when all of its buttons are down within 1.5 s of power-up and stay down for `hold`; letting go earlier cancels it. The unlock combo is held while the device's 30 s unlock window is open. `factory_reset_combo` must differ from `boot_combo`: neither may contain the other, or one would set off the other (sharing a button is fine). What the status LED and a display show meanwhile: [core/README.md "Factory-reset combo"](../README.md#factory-reset-combo).
- **Blink codes.** The status LED's patterns, and which code shows when there are several: [core/README.md "Status LED"](../README.md#status-led).
- **Flash writes.** A write can stop the CPU, and the key scan with it, for 20-40 ms, so the device's own writes are off unless `write_to_flash` turns them on, and then wait for a quiet moment. The host's saves always work. What each item stores and when: [core/README.md "Writes the device makes on its own"](../README.md#writes-the-device-makes-on-its-own).
