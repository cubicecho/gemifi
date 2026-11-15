// What has been checked on real hardware, per device YAML in devices/, for
// /docs/devices/. Every devices/*.yaml needs an entry here (the site tests
// fail without one), so a new example can't go up without saying whether
// anyone has run it. The name, platform and chip come from the YAML itself.
//
// `status` is a key of statuses.js: "works" = checked on hardware (`checked`
// is the date of the last check), "in-progress" = builds, not checked yet.
// `device` is the slug of a page in src/devices/, if one covers it. `issue`
// is the hardware test tracker.

import { readRepo } from "../../lib/repo-docs.js";

/** A device YAML's top-level `key: value` lines (name, platform, chip ...), comments dropped. */
function yamlHeader(file) {
  const out = {};
  for (const m of readRepo(file).matchAll(/^([a-z_]+):[ \t]+([^#\n]*?)[ \t]*(?:#.*)?$/gm)) {
    out[m[1]] ??= m[2].replace(/^["']|["']$/g, "");
  }
  return out;
}

const TESTED = [
  {
    yaml: "devices/keystation-mini32-mk3.yaml",
    device: "keystation-mini32-mk3",
    status: "works",
    checked: "2026-10-31",
    works: "Keys with velocity, buttons, the volume knob, LEDs, settings from the web page, app updates over USB",
    pending: "Presets and the Edit mode, DAW feedback, the boot-loop guard",
    issue: 7,
  },
  {
    yaml: "devices/m-audio-code61.yaml",
    device: "m-audio-code",
    status: "works",
    checked: "2026-11-11",
    works:
      "Keys, buttons, encoders, faders, wheels, aftertouch, XY pad, pads and their LEDs, the display, DIN MIDI, presets and settings, app updates over USB; Shift + Up/Down programs and the Zynthian presets",
    pending: "A power cut during a save, the newer layers",
    issue: 24,
  },
  {
    yaml: "devices/m-audio-code61-ledmap.yaml",
    device: "m-audio-code",
    status: "in-progress",
    works: "",
    pending: "A bring-up build for finding which TM1628 bit lights which LED; not a controller",
    issue: 24,
  },
  {
    yaml: "devices/diy-esp32-pot-oled.yaml",
    device: "diy-esp32",
    status: "works",
    checked: "2026-11-12",
    works:
      "The OLED, the pot, BLE MIDI to a Zynthian (7.5 ms interval), app updates over serial from the web updater, rollback after a failed boot",
    pending: "Settings kept over a restart",
    issue: 25,
  },
  {
    yaml: "devices/diy-esp32s3-pot-oled.yaml",
    device: "diy-esp32",
    status: "in-progress",
    works: "",
    pending: "USB MIDI on the S3",
    issue: 25,
  },
  {
    yaml: "devices/esp32-example.yaml",
    device: "diy-esp32",
    status: "in-progress",
    works: "",
    pending: "Everything",
    issue: 25,
  },
  {
    yaml: "devices/diy-esp32s3-faders.yaml",
    device: "diy-esp32",
    status: "in-progress",
    works: "",
    pending: "Everything",
    issue: 25,
  },
  {
    yaml: "devices/diy-esp32s3-pads-ws2812.yaml",
    device: "diy-esp32",
    status: "in-progress",
    works: "",
    pending: "Everything",
    issue: 25,
  },
  {
    yaml: "devices/diy-bluepill.yaml",
    status: "in-progress",
    works: "",
    pending: "Everything",
  },
  {
    yaml: "devices/diy-floorboard.yaml",
    status: "in-progress",
    works: "",
    pending: "Everything",
  },
];

/** Each entry with its YAML's name, friendly name, platform, chip and board; checked ones first. */
export default function () {
  return TESTED.map((t) => {
    const h = yamlHeader(t.yaml);
    return {
      ...t,
      file: t.yaml.split("/").pop(),
      name: h.friendly_name ?? h.name,
      platform: h.platform,
      chip: h.chip,
      board: h.board,
    };
  }).sort((a, b) => Number(b.status === "works") - Number(a.status === "works"));
}
