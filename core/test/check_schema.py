"""Parse the firmware's own schema blob with the host tool's parser."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "host"))
import gemifi_proto as gp  # noqa: E402

s = gp.parse_schema(Path(sys.argv[1]).read_bytes())
names = [p.name for p in s.params]
expect = ["midi.channel", "midi.transpose", "midi.octave", "velocity.curve", "velocity.fixed",
          "velocity.fast", "velocity.slow", "button.action", "button.number", "bend.glide",
          "ramp.time", "analog.cc", "debug.events", "debug.log_level"]
assert names == expect, names
act = next(p for p in s.params if p.name == "button.action")
assert act.count == 7 and act.index_labels[4] == "Pitch down" and act.defaults == [1, 2, 5, 6, 9, 10, 3], act
assert next(p for p in s.params if p.name == "bend.glide").unit == "ms"
print(f"schema ok: {len(names)} params, {len(sys.argv[1]) and Path(sys.argv[1]).stat().st_size} bytes")
