# MIDI and USB

The keys that say how the device reaches the computer and other gear: its USB ids, its MIDI transport, the 5-pin DIN jacks, and the MIDI message kinds it can send beyond notes and CCs.

```yaml
usb: { vid: 0x1209, pid: 0x0002 }
midi: { transport: usb }
din_midi: { tx_pin: PA9, rx_pin: PA10, mode: usb_merge }
midi_messages:
  enable: [cc14, nrpn]
  sysex:
    - { id: gs_volume, data: "F0 41 10 42 12 [40 00 04 v] cs F7" }
```

<!-- keys -->

- **USB ids.** 1209:0002 is the pid.codes test pair: fine on your own bench (the builder warns when `usb:` is left out), but a device you hand out wants ids of its own. `usb:` only matters with `midi.transport: usb`; with another transport the builder ignores it and warns.
- **The DAW port.** With `mcu:`, the device gets a second USB-MIDI port for Mackie Control, unless `usb.daw_port: false` ([core/README.md "Second USB port and Mackie Control"](../README.md#second-usb-port-and-mackie-control)). `din_midi` `separate_port` needs that second port too, so the two can't be combined.
- **DIN modes.** What the computer sees in each mode, how DIN Out mixes the device, the thru and USB, running status, coalescing and the limits: [core/README.md "DIN MIDI"](../README.md#din-midi). `thru`, `controller_out` and `usb_to_din` are only the start values: the web page changes them later. On stm32f1, `dma: true` competes with the hardware scans for DMA channels ([core/README.md "DMA channels (stm32f1)"](../README.md#dma-channels-stm32f1)).
- **Message kinds.** Which control modes and actions send them, SysEx placeholders and checksums, what they cost and what comes back: [core/README.md "MIDI messages"](../README.md#midi-messages).
