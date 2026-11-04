"""Parse the firmware's own schema blob with the host tool's parser."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "host"))
import gemifi_proto as gp  # noqa: E402

s = gp.parse_schema(Path(sys.argv[1]).read_bytes())
names = [p.name for p in s.params]
expect = ["midi.channel", "midi.transpose", "midi.octave", "velocity.curve", "velocity.fixed",
          "velocity.fast", "velocity.slow", "button.action", "button.number", "bend.glide",
          "ramp.time", "analog.cc", "analog.mode", "debug.events", "debug.log_level",
          "preset.active", "preset.program", "preset.dirty", "preset.count"]
assert names == expect, names
# schema flags bit 5 = PRESET (per-preset param, research/core/presets-design.md §1)
F_PRESET = 0x20
per_preset = {p.name for p in s.params if p.flags & F_PRESET}
assert per_preset == {"midi.channel", "midi.transpose", "midi.octave", "velocity.curve", "velocity.fixed",
                      "button.action", "button.number", "bend.glide", "ramp.time", "analog.cc", "analog.mode",
                      "preset.program"}, per_preset
active = next(p for p in s.params if p.name == "preset.active")
assert (active.min, active.max) == (1, 8), active
dirty = next(p for p in s.params if p.name == "preset.dirty")
assert dirty.read_only and dirty.volatile, dirty
act = next(p for p in s.params if p.name == "button.action")
assert act.count == 7 and act.index_labels[4] == "Pitch down" and act.defaults == [1, 0, 5, 6, 9, 10, 3], act   # Edit (index 1) is the mode button: no action
assert act.options[13:] == ["Preset next", "Preset previous", "Preset select"] and act.max == 15, act.options
assert next(p for p in s.params if p.name == "bend.glide").unit == "ms"
print(f"schema ok: {len(names)} params, {len(sys.argv[1]) and Path(sys.argv[1]).stat().st_size} bytes")
