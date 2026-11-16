# DAW feedback

The keys that say what the device does with the MIDI a host (a DAW) sends back: plain feedback for LEDs, toggles, encoders and pick-up, DAW scripts, and Mackie Control.

```yaml
feedback:
  led_owner: host_then_local
  led_timeout: 2s
mcu:
  extender: false
```

<!-- keys -->

- **What the host's messages do.** Which controls follow which messages, how to turn the host's output on in Live, and the per-LED `feedback:` sources: [core/README.md "Plain feedback (F1, every DAW)"](../README.md#plain-feedback-f1-every-daw). The protocol as implemented is [protocol/host-feedback.md](../../protocol/host-feedback.md).
- **LED owners.** An LED shows its own state, the host's, or the host's for a while after each message; `led_owner` and `led_timeout` are every LED's default, and an LED's `owner:` and `timeout:` set its own ([core/README.md "LED ownership"](../README.md#led-ownership)).
- **Mackie Control.** `mcu:`, even empty, turns it on. On USB it gets a second port (see `usb.daw_port`, [MIDI and USB](midi.md)); on serial and BLE, with one port, plain feedback is off and the whole device speaks MCU. The names a control takes and the messages they send: [core/README.md "Second USB port and Mackie Control"](../README.md#second-usb-port-and-mackie-control).
- **DAW scripts.** A script inside the DAW drives the device by component index; what it can ask and set, and `daw:` roles: [core/README.md "DAW scripts (F3)"](../README.md#daw-scripts-f3).
- **Cost.** Plain feedback is about 0.5 KB of flash and 24 B of RAM on a NUC120, scripts about 1.5 KB, `mcu:` about 0.5 KB more; `enabled: false` without `mcu:` compiles it all out ([core/README.md "DAW feedback"](../README.md#daw-feedback)).
