# Device

The keys that say what the file is: its format, the device's names and version, and the platform, chip and board it builds for.

```yaml
gemifi: 2
name: keystation-mini32
friendly_name: Keystation Mini 32
project: { version: 0.1.3 }
board_id: 0x0001
platform: nuvoton-nuc1xx
chip: NUC120LC1BN
board: keystation-mini32-mk3
```

<!-- keys -->

- **Names.** `name` names the built images and is what a dashboard or CLI calls the device. `friendly_name` is what people see. Component ids and names follow the same rules everywhere ([core/README.md "Device YAML"](../README.md#device-yaml), "Ids and names").
- **The board.** A board profile fixes the board's `board_id` and reserves its pins, so a YAML for that board can't use them twice ([core/README.md "Board profiles"](../README.md#board-profiles)). A DIY board takes an id from 0x3F00..0x3FFF.
- **The platform.** What each one supports, and how to flash it, is on its page under Platforms.
