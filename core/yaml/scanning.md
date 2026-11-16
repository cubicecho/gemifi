# Scanning

The keys that say how often the device reads its controls, and whether the CPU or the hardware (timers and DMA) does the reading.

```yaml
scan_interval: 125us
hardware_scan:
  matrix: { row_time: 3.5us, settle: 3us, precharge: true }
  analog: true
```

<!-- keys -->

- **The scan tick.** Without `hardware_scan:` the key matrix is scanned from the tick, a row at a time, with each component's own `settle` ([core/README.md "Software scan"](../README.md#software-scan)). A shorter `scan_interval` times the keys more finely and costs more CPU: [performance.md](../performance.md) has the numbers per platform and per component.
- **Hardware scan.** stm32f1 only for now. What a matrix has to look like for it (one store selects a row, one register reads the columns), how frames replace the tick for key timing, when precharge is safe, and what the analog sweeps read: [core/README.md "Hardware scan"](../README.md#hardware-scan). A board that doesn't fit logs a warning at boot and scans in software.
- **The checks.** `row_time` has to fit the settle and the DMA steps' gaps: at least `settle` + 0.25us, or + 0.5us with `precharge`. `hardware_scan.matrix` needs a `key_matrix:` or `switch_matrix:` to scan, and `analog: true` needs analog inputs or pads.
- **Timers and DMA channels.** Which timer each scan can use, how `auto` picks one, and what fits next to `din_midi` `dma: true` and an LED strip: [core/README.md "DMA channels (stm32f1)"](../README.md#dma-channels-stm32f1).
