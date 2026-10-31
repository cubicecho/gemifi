"""Config support (sysex.md 4.3, 4.4): s32, the schema codec, config frames, value parsing, and the CLI
config commands against a simulated app (tests/sim_device.py)."""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
import make_test_vectors  # noqa: E402
from sim_device import SimDevice, SimPort  # noqa: E402

SCHEMA = make_test_vectors.example_schema()


class TestS32(unittest.TestCase):
    def test_known(self):
        self.assertEqual(gp.s32(-1), bytes([0x7F, 0x7F, 0x7F, 0x7F, 0x0F]))
        self.assertEqual(gp.s32(0), bytes(5))
        self.assertEqual(gp.s32(-(1 << 31)), bytes([0, 0, 0, 0, 0x08]))
        self.assertEqual(gp.s32((1 << 31) - 1), bytes([0x7F, 0x7F, 0x7F, 0x7F, 0x07]))

    def test_round_trip(self):
        for v in (0, 1, -1, 127, 128, -128, -8192, 2000, 0x7FFFFFFF, -0x80000000):
            self.assertEqual(gp.dec_s32(gp.s32(v)), v)

    def test_range(self):
        for v in (1 << 31, -(1 << 31) - 1):
            with self.assertRaises(ValueError):
                gp.s32(v)
        with self.assertRaises(gp.ProtocolError):
            gp.dec_s32(bytes([0, 0, 0, 0, 0x10]))
        with self.assertRaises(gp.ProtocolError):
            gp.dec_s32(bytes(4))


class TestSchema(unittest.TestCase):
    def test_round_trip(self):
        raw = gp.build_schema(SCHEMA)
        s = gp.parse_schema(raw)
        self.assertEqual(s, SCHEMA)
        self.assertEqual(gp.build_schema(s), raw)
        self.assertEqual(gp.Schema.from_json(json.loads(json.dumps(s.to_json()))), SCHEMA)

    def test_example_fields(self):
        s = gp.parse_schema(gp.build_schema(SCHEMA))
        self.assertEqual(s.groups, ["MIDI", "Velocity", "Knob", "Buttons", "Debug"])
        act = s.by_name("button.action")
        self.assertEqual((act.id, act.type_name, act.count, act.min, act.max), (9, "enum", 7, 0, 12))
        self.assertEqual(act.flags, gp.F_PER_INDEX_DEFAULT | gp.F_INDEX_LABELS)
        self.assertEqual(act.defaults, [1, 2, 5, 6, 9, 10, 3])
        self.assertEqual(act.index_labels[2], "Oct -")
        self.assertEqual(act.options[act.default(6)], "CC ramp")
        self.assertEqual(s.by_id(2).min, -24)
        self.assertEqual(s.by_name("velocity.curve").max, 3)
        glide = s.by_name("bend.glide")
        self.assertEqual((glide.unit, glide.flags), ("ms", gp.F_HAS_UNIT))
        self.assertEqual(s.by_name("debug.events").type_name, "bool")

    def layout(self):
        """A one-param schema built by hand with struct, independently of build_schema."""
        def st(t):
            return bytes([len(t)]) + t.encode()
        return (struct.pack("<BBH", 1, 1, 1) + st("G") +
                struct.pack("<HBBHBiii", 0x3FFF, 0, 0, 300, 0, -5, 5, -2) + st("a.b") + st("A B"))

    def test_hand_layout(self):
        s = gp.parse_schema(self.layout())
        p = s.params[0]
        self.assertEqual((p.id, p.count, p.min, p.max, p.defaults, p.name, p.label, p.unit),
                         (0x3FFF, 300, -5, 5, [-2], "a.b", "A B", ""))
        self.assertEqual(p.default(299), -2)
        self.assertEqual(gp.build_schema(s), self.layout())

    def test_unknown_flags_and_types(self):
        raw = bytearray(self.layout())
        flags_at = 4 + 2 + 2 + 1 + 1 + 2
        raw[flags_at] = 0xE0          # unknown flag bits: ignored (no extra data read)
        raw[flags_at - 4] = 9         # unknown type: an int, no enum trailer
        p = gp.parse_schema(bytes(raw)).params[0]
        self.assertEqual((p.flags, p.type, p.type_name, p.options), (0xE0, 9, "int", []))
        self.assertEqual(gp.format_value(p, -3), "-3")
        self.assertEqual(gp.parse_value(p, "4"), 4)

    def test_truncated(self):
        raw = gp.build_schema(SCHEMA)
        for n in range(len(raw)):
            with self.assertRaisesRegex(gp.ProtocolError, "truncated"):
                gp.parse_schema(raw[:n])

    def test_errors(self):
        raw = gp.build_schema(SCHEMA)
        with self.assertRaisesRegex(gp.ProtocolError, "version 2"):
            gp.parse_schema(bytes([2]) + raw[1:])
        bad = bytearray(self.layout())
        bad[4 + 2 + 2 + 1] = 3        # group 3 of 1
        with self.assertRaisesRegex(gp.ProtocolError, "group"):
            gp.parse_schema(bytes(bad))
        bad = bytearray(self.layout())
        bad[10:12] = b"\x00\x00"      # count 0
        with self.assertRaisesRegex(gp.ProtocolError, "count"):
            gp.parse_schema(bytes(bad))

    def test_encoder_rejects_inconsistent(self):
        p = gp.make_param(1, "x", gp.T_INT, 0, 0, 10, [1, 2], count=3)
        with self.assertRaisesRegex(ValueError, "defaults"):
            gp.build_schema(gp.Schema(1, ["G"], [p]))
        p = gp.make_param(1, "x" * 64, gp.T_INT, 0, 0, 10)
        with self.assertRaisesRegex(ValueError, "63"):
            gp.build_schema(gp.Schema(1, ["G"], [p]))
        p = gp.make_param(1, "x", gp.T_INT, 1, 0, 10)
        with self.assertRaises(ValueError):
            gp.build_schema(gp.Schema(1, ["G"], [p]))


class TestFrames(unittest.TestCase):
    def test_numbers(self):
        self.assertEqual((gp.GET_SCHEMA, gp.GET_PARAM, gp.SET_PARAM, gp.COMMIT, gp.FACTORY_RESET, gp.GET_PARAMS,
                          gp.SET_PARAMS, gp.PARAM_CHANGED), (2, 3, 4, 7, 8, 9, 10, 0x62))
        self.assertEqual((gp.BAD_PARAM, gp.BAD_VALUE), (10, 11))
        for c in (gp.COMMIT, gp.FACTORY_RESET, gp.FW_BEGIN, gp.FW_END):
            self.assertIn(c, gp.LONG_TIMEOUT_CMDS)

    def test_param_replies(self):
        r = gp.parse_param_reply(gp.build_param_reply(9, 3, -7))
        self.assertEqual((r.id, r.index, r.value), (9, 3, -7))
        f = gp.parse_frame(gp.build_frame(gp.GET_PARAM | gp.REPLY, 4, gp.build_param_reply(9, 3, -7)))
        self.assertTrue(gp.reply_matches(f, gp.GET_PARAM, 4))
        f = gp.parse_frame(gp.build_frame(gp.GET_PARAM | gp.REPLY, 4, b"\x00" + gp.u14(9)))
        self.assertFalse(gp.reply_matches(f, gp.GET_PARAM, 4))   # OK but short
        self.assertEqual(gp.build_param_reply(1, 0, 5, status=gp.BAD_VALUE), bytes([gp.BAD_VALUE]))

    def test_params_replies(self):
        self.assertEqual(gp.parse_params_reply(gp.build_params_reply(10, 2, [1, -2, 3])), (10, 2, [1, -2, 3]))
        with self.assertRaises(gp.ProtocolError):
            gp.parse_params_reply(gp.build_params_reply(10, 2, [1, -2, 3])[:-1])
        self.assertEqual(gp.parse_set_params_reply(gp.build_set_params_reply(9, 2, 3)), (9, 2, 3))
        with self.assertRaises(ValueError):
            gp.req_get_params(1, 0, 17)
        with self.assertRaises(ValueError):
            gp.req_set_params(1, 0, [])

    def test_schema_reply(self):
        raw = gp.build_schema(SCHEMA)
        total, off, data = gp.parse_schema_reply(gp.build_schema_reply(raw, 64, 64))
        self.assertEqual((total, off, data), (len(raw), 64, raw[64:128]))
        self.assertEqual(gp.parse_schema_reply(gp.build_schema_reply(raw, len(raw), 64))[2], b"")
        self.assertEqual(gp.req_get_schema(0x12345, 64), gp.u28(0x12345) + gp.u14(64))
        for bad in (0, 65):
            with self.assertRaises(ValueError):
                gp.req_get_schema(0, bad)

    def test_param_changed_and_encoder(self):
        m = gp.parse_param_changed(gp.build_param_changed(3, 0, -1))
        self.assertEqual((m.id, m.index, m.value), (3, 0, -1))
        self.assertEqual(gemifi.describe_param_changed(m), "PARAM_CHANGED id 3[0] = -1")
        self.assertEqual(gemifi.describe_param_changed(m, SCHEMA), "PARAM_CHANGED midi.octave = -1")
        m = gp.ParamValue(9, 2, 6)
        self.assertEqual(gemifi.describe_param_changed(m, SCHEMA), "PARAM_CHANGED button.action:2 = Octave up")
        e = gp.parse_event(gp.build_event(gp.EV_ENCODER, 1, -3 & 0x3FFF, 5))
        self.assertEqual(e.delta, -3)
        self.assertIn("encoder 1 = -3", str(e))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            gemifi.print_unsolicited(gp.parse_frame(gp.build_frame(gp.PARAM_CHANGED, 1, gp.build_param_changed(3, 0, 2))))
        self.assertEqual(out.getvalue().strip(), "PARAM_CHANGED id 3[0] = 2")


class TestValues(unittest.TestCase):
    def p(self, name):
        return SCHEMA.by_name(name)

    def test_notes(self):
        self.assertEqual(gp.parse_note("C4"), 60)
        self.assertEqual(gp.parse_note("c4"), 60)
        self.assertEqual(gp.parse_note("F#4"), 66)
        self.assertEqual(gp.parse_note("Bb3"), 58)
        self.assertEqual(gp.parse_note("C-1"), 0)
        self.assertEqual(gp.parse_note("G9"), 127)
        for bad in ("H4", "C", "G#9", "C-2", "C#", "4", "C--1", "Cx4"):
            with self.assertRaises(ValueError, msg=bad):
                gp.parse_note(bad)
        self.assertEqual([gp.note_name(n) for n in (0, 59, 60, 61, 127)], ["C-1", "B3", "C4", "C#4", "G9"])
        note = gp.make_param(20, "pad.note", gp.T_NOTE, 0, 0, 127, 36)
        self.assertEqual(gp.parse_value(note, "D#2"), 39)
        self.assertEqual(gp.parse_value(note, "39"), 39)
        self.assertEqual(gp.format_value(note, 60), "C4 (60)")

    def test_enum(self):
        p = self.p("button.action")
        self.assertEqual(gp.parse_value(p, "cc TOGGLE"), 2)
        self.assertEqual(gp.parse_value(p, "Program previous"), 12)
        self.assertEqual(gp.parse_value(p, "4"), 4)
        self.assertEqual(gp.format_value(p, 9), "Pitch bend down")
        self.assertEqual(gp.format_value(p, 99), "99")
        with self.assertRaisesRegex(ValueError, "options: None, CC momentary"):
            gp.parse_value(p, "Jump")
        with self.assertRaisesRegex(ValueError, "outside"):
            gp.parse_value(p, "13")
        shifted = gp.make_param(30, "x", gp.T_ENUM, 0, -1, None, -1, options=["Off", "A", "B"])
        self.assertEqual((shifted.max, gp.parse_value(shifted, "b"), gp.format_value(shifted, -1)), (1, 1, "Off"))

    def test_int_bool_unit(self):
        self.assertEqual(gp.parse_value(self.p("midi.transpose"), "-12"), -12)
        self.assertEqual(gp.parse_value(self.p("knob.cc"), "0x10"), 16)
        with self.assertRaisesRegex(ValueError, "outside 1..16"):
            gp.parse_value(self.p("midi.channel"), "17")
        self.assertEqual(gp.parse_value(self.p("midi.channel"), "17", check_range=False), 17)
        b = self.p("debug.events")
        self.assertEqual([gp.parse_value(b, t) for t in ("on", "OFF", "true", "0", "yes")], [1, 0, 1, 0, 1])
        self.assertEqual(gp.format_value(b, 1), "on")
        g = self.p("bend.glide")
        self.assertEqual([gp.parse_value(g, t) for t in ("80", "80ms", "80 MS")], [80, 80, 80])
        self.assertEqual((gp.format_value(g, 80), gp.format_value(g, 80, unit=False)), ("80 ms", "80"))

    def test_refs(self):
        self.assertEqual(gp.find_param(SCHEMA, "midi.channel"), (self.p("midi.channel"), None))
        self.assertEqual(gp.find_param(SCHEMA, "9:3"), (self.p("button.action"), 3))
        self.assertEqual(gp.find_param(SCHEMA, "button.number:oct +"), (self.p("button.number"), 3))
        for bad in ("nope", "button.action:7", "button.action:Foo", "99"):
            with self.assertRaises(ValueError, msg=bad):
                gp.find_param(SCHEMA, bad)

    def test_assignments(self):
        pa = gemifi.parse_assignment
        self.assertEqual(pa(SCHEMA, "button.action:Sustain=cc toggle"), (self.p("button.action"), 0, 2))
        self.assertEqual(pa(SCHEMA, "midi.transpose=-3"), (self.p("midi.transpose"), 0, -3))
        self.assertEqual(pa(SCHEMA, "1=16"), (self.p("midi.channel"), 0, 16))
        with self.assertRaisesRegex(ValueError, "INDEX"):
            pa(SCHEMA, "button.number=3")
        with self.assertRaisesRegex(ValueError, "NAME"):
            pa(SCHEMA, "midi.channel")
        ro = gp.Schema(1, ["G"], [gp.make_param(1, "fw.build", gp.T_INT, 0, 0, 9, 0, flags=gp.F_READ_ONLY)])
        with self.assertRaisesRegex(ValueError, "read-only"):
            pa(ro, "fw.build=1")


class FakeBackend:
    label = "sim"

    def __init__(self, dev):
        self.dev = dev
        self.port = SimPort(dev)

    def ports(self):
        return ["Keystation Mini 32 MK3 (gemifi) [hw:2,0]"]

    def open(self, name):
        return self.port


def big_schema():
    """An array longer than one GET_PARAMS/SET_PARAMS run (16)."""
    return gp.Schema(1, ["Keys"], [
        gp.make_param(1, "key.note", gp.T_NOTE, 0, 0, 127, list(range(21, 21 + 40)), count=40),
        gp.make_param(2, "key.curve", gp.T_ENUM, 0, 0, None, 0, options=["Lin", "Log"]),
    ])


class TestCli(unittest.TestCase):
    def run_cli(self, dev, *argv):
        a = gemifi.build_parser().parse_args(list(argv))
        backend = FakeBackend(dev)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            rc = gemifi.COMMANDS[a.cmd](a, backend)
        return rc, out.getvalue(), backend.port

    def dev(self, **kw):
        return SimDevice(role=gp.ROLE_APP, schema=kw.pop("schema", SCHEMA), **kw)

    def test_schema(self):
        rc, out, _ = self.run_cli(self.dev(), "schema")
        self.assertEqual(rc, 0)
        self.assertIn("button.action", out)
        self.assertIn("12=Program previous", out)
        self.assertIn("ms", out)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "s.bin")
            rc, out, _ = self.run_cli(self.dev(), "schema", "--json", "--raw", path)
            with open(path, "rb") as f:
                self.assertEqual(f.read(), gp.build_schema(SCHEMA))
        self.assertEqual(json.loads(out), SCHEMA.to_json())

    def test_schema_windows(self):
        dev = self.dev()
        port = SimPort(dev)
        s = gemifi.Session(port, timeout=0.005)
        raw = s.get_schema_bytes()
        self.assertEqual(raw, gp.build_schema(SCHEMA))
        reqs = [gp.parse_frame(m) for m in port.sent]
        self.assertEqual([gp.dec_u28(f.payload[:4]) for f in reqs], list(range(0, len(raw), 64)))
        self.assertTrue(all(gp.dec_u14(f.payload[4:6]) == 64 for f in reqs))

    def test_bootloader_refused(self):
        with self.assertRaisesRegex(gemifi.GemifiError, "bootloader"):
            self.run_cli(SimDevice(role=gp.ROLE_BOOT), "get")
        with self.assertRaisesRegex(gemifi.GemifiError, "config"):
            self.run_cli(SimDevice(role=gp.ROLE_APP), "get")

    def test_get_all(self):
        rc, out, _ = self.run_cli(self.dev(), "get")
        self.assertEqual(rc, 0)
        self.assertIn("button.action:2", out)
        self.assertRegex(out, r"button\.action:2\s+Octave down\s+Button action / Oct -")
        self.assertRegex(out, r"velocity\.curve\s+Normal")
        self.assertRegex(out, r"bend\.glide\s+80 ms")
        self.assertRegex(out, r"debug\.events\s+off")

    def test_get_some(self):
        rc, out, _ = self.run_cli(self.dev(), "get", "midi.channel", "button.number:Mod", "9")
        lines = out.strip().splitlines()
        self.assertEqual(len(lines), 1 + 1 + 7)
        self.assertRegex(lines[1], r"button\.number:6\s+1\s")

    def test_set(self):
        dev = self.dev()
        rc, out, _ = self.run_cli(dev, "set", "midi.transpose=-5", "button.action:Edit=cc toggle", "--commit")
        self.assertEqual(rc, 0)
        self.assertEqual((dev.values[(2, 0)], dev.values[(9, 1)]), (-5, 2))
        self.assertEqual(dev.commits, 1)
        self.assertIn("button.action:1 = CC toggle", out)

    def test_set_parse_error_sends_nothing(self):
        dev = self.dev()
        with self.assertRaisesRegex(gemifi.GemifiError, "outside"):
            self.run_cli(dev, "set", "midi.transpose=-5", "midi.channel=0")
        self.assertEqual(dev.values[(2, 0)], 0)
        self.assertNotIn("SET_PARAM", dev.log)

    def test_commit_and_factory_reset(self):
        dev = self.dev()
        self.run_cli(dev, "set", "midi.octave=2")
        self.assertEqual(self.run_cli(dev, "commit")[0], 0)
        self.assertEqual(dev.stored[(3, 0)], 2)
        self.assertEqual(self.run_cli(dev, "factory-reset", "--yes")[0], 0)
        self.assertEqual((dev.values[(3, 0)], dev.stored), (0, None))

    def test_factory_reset_asks(self):
        dev = self.dev()
        dev.values[(3, 0)] = 2
        old = sys.stdin
        try:
            sys.stdin = io.StringIO("no\n")
            rc, out, _ = self.run_cli(dev, "factory-reset")
        finally:
            sys.stdin = old
        self.assertEqual((rc, dev.values[(3, 0)]), (1, 2))
        self.assertNotIn("FACTORY_RESET", dev.log)

    def test_backup_restore(self):
        for bulk in (True, False):
            src = self.dev(bulk=bulk)
            src.values.update({(2, 0): -7, (9, 4): 12, (11, 0): 1500})
            with tempfile.TemporaryDirectory() as d:
                path = os.path.join(d, "b.json")
                self.assertEqual(self.run_cli(src, "backup", path)[0], 0)
                with open(path) as f:
                    doc = json.load(f)
                self.assertEqual((doc["board_id"], doc["fw_version"]), (1, "0.1.0"))
                self.assertEqual(doc["values"]["midi.transpose"], -7)
                self.assertEqual(doc["values"]["button.action"], [1, 2, 5, 6, 12, 10, 3])
                dst = self.dev(bulk=bulk)
                rc, out, _ = self.run_cli(dst, "restore", path, "--commit")
                self.assertEqual(rc, 0, out)
                self.assertEqual(dst.values, src.values)
                self.assertEqual(dst.commits, 1)
                # two array params; without bulk support the host tries once, gets BAD_CMD, then goes single
                self.assertEqual(src.log.count("GET_PARAMS"), 2 if bulk else 1)
                self.assertEqual(dst.log.count("SET_PARAMS"), 2 if bulk else 1)

    def test_restore_reports_per_entry(self):
        dev = self.dev()
        doc = {"board_id": 1, "values": {"midi.channel": 5, "gone.param": 3, "midi.transpose": 99,
                                         "button.number": [1, 2, 300, 4, 5, 6, 7], "velocity.curve": "hard",
                                         "button.action": [0, 0]}}
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "b.json")
            with open(path, "w") as f:
                json.dump(doc, f)
            rc, out, _ = self.run_cli(dev, "restore", path)
        self.assertEqual(rc, 1)
        self.assertIn("gone.param: not in this firmware's schema", out)
        self.assertIn("midi.transpose: BAD_VALUE", out)
        self.assertIn("button.number:2: BAD_VALUE", out)
        self.assertIn("backup has 2 values, device has 7", out)
        self.assertEqual(dev.values[(1, 0)], 5)
        self.assertEqual(dev.values[(4, 0)], 2)
        self.assertEqual([dev.values[(10, i)] for i in range(7)], [1, 2, 0, 4, 5, 6, 7])  # others still applied
        self.assertEqual([dev.values[(9, i)] for i in range(3)], [0, 0, 5])

    def test_runs_of_16(self):
        sch = big_schema()
        dev = self.dev(schema=sch)
        s = gemifi.Session(SimPort(dev), timeout=0.005)
        p = sch.by_name("key.note")
        self.assertEqual(s.get_values(p), list(range(21, 61)))
        self.assertEqual(dev.log.count("GET_PARAMS"), 3)
        n, err = gemifi.restore_values(s, sch, {"key.note": list(range(40, 80))}, warn=lambda m: None)
        self.assertEqual((n, err), (40, 0))
        self.assertEqual(dev.log.count("SET_PARAMS"), 3)
        self.assertEqual([dev.values[(1, i)] for i in range(40)], list(range(40, 80)))

    def test_bulk_fallback(self):
        dev = self.dev(schema=big_schema(), bulk=False)
        s = gemifi.Session(SimPort(dev), timeout=0.005)
        self.assertEqual(len(s.get_values(dev.schema.params[0])), 40)
        self.assertFalse(s.bulk)
        self.assertEqual(dev.log.count("GET_PARAMS"), 1)     # tried once, then single GET_PARAMs
        self.assertEqual(dev.log.count("GET_PARAM"), 40)


@unittest.skipUnless(os.path.exists(make_test_vectors.DEFAULT_OUT), "protocol/test-vectors.json not generated")
class TestConfigVectors(unittest.TestCase):
    """The config section of protocol/test-vectors.json, decoded with the codec."""

    @classmethod
    def setUpClass(cls):
        with open(make_test_vectors.DEFAULT_OUT) as f:
            cls.v = json.load(f)["config"]

    def test_s32(self):
        self.assertEqual([t["value"] for t in self.v["s32"]], [0, 1, -1, 127, -8192, 2147483647, -2147483648])
        for t in self.v["s32"]:
            self.assertEqual(gp.s32(t["value"]), bytes.fromhex(t["bytes"]))
            self.assertEqual(gp.dec_s32(bytes.fromhex(t["bytes"])), t["value"])

    def test_schema(self):
        raw = bytes.fromhex(self.v["schema"]["raw"])
        self.assertEqual(len(raw), self.v["schema"]["size"])
        self.assertEqual(gp.parse_schema(raw).to_json(), self.v["schema"]["parsed"])
        for t in self.v["bad_schemas"]:
            with self.assertRaisesRegex(gp.ProtocolError, t["error"], msg=t["desc"]):
                gp.parse_schema(bytes.fromhex(t["raw"]))

    def test_frames(self):
        for t in self.v["frames"]:
            f = gp.parse_frame(bytes.fromhex(t["frame"]))
            self.assertEqual((f.cmd, f.seq, f.payload.hex(" ").upper()), (t["cmd"], t["seq"], t["payload"]), t["desc"])
            fl = t.get("fields", {})
            base = f.cmd & ~gp.REPLY if f.is_reply else f.cmd
            if f.is_reply:
                self.assertEqual(f.status, fl["status"], t["desc"])
                self.assertTrue(gp.reply_matches(f, base, f.seq), t["desc"])
                if f.status != gp.OK:
                    continue
            if f.cmd in (gp.GET_PARAM | gp.REPLY, gp.SET_PARAM | gp.REPLY):
                r = gp.parse_param_reply(f.payload)
                self.assertEqual((r.id, r.index, r.value), (fl["id"], fl["index"], fl["value"]))
            elif f.cmd == gp.GET_SCHEMA | gp.REPLY:
                total, off, data = gp.parse_schema_reply(f.payload)
                self.assertEqual((total, off, data.hex(" ").upper()), (fl["total"], fl["offset"], fl["data"]))
            elif f.cmd == gp.GET_PARAMS | gp.REPLY:
                self.assertEqual(gp.parse_params_reply(f.payload), (fl["id"], fl["index"], fl["values"]))
            elif f.cmd == gp.SET_PARAMS | gp.REPLY:
                self.assertEqual(gp.parse_set_params_reply(f.payload), (fl["id"], fl["index"], fl["n"]))
            elif f.cmd == gp.PARAM_CHANGED:
                m = gp.parse_param_changed(f.payload)
                self.assertEqual((m.id, m.index, m.value), (fl["id"], fl["index"], fl["value"]))
            elif f.cmd == gp.EVENT:
                e = gp.parse_event(f.payload)
                self.assertEqual((e.type, e.index, e.value, e.delta), (fl["type"], fl["index"], fl["value"], fl["delta"]))
            elif f.cmd == gp.GET_SCHEMA:
                self.assertEqual(f.payload, gp.req_get_schema(fl["offset"], fl["max"]))
            elif f.cmd in (gp.GET_PARAM,):
                self.assertEqual(f.payload, gp.req_get_param(fl["id"], fl["index"]))
            elif f.cmd == gp.SET_PARAM:
                self.assertEqual(f.payload, gp.req_set_param(fl["id"], fl["index"], fl["value"]))
            elif f.cmd == gp.GET_PARAMS:
                self.assertEqual(f.payload, gp.req_get_params(fl["id"], fl["index"], fl["n"]))
            elif f.cmd == gp.SET_PARAMS:
                self.assertEqual(f.payload, gp.req_set_params(fl["id"], fl["index"], fl["values"]))
            elif f.cmd in (gp.COMMIT, gp.FACTORY_RESET):
                self.assertEqual(f.payload, b"")

    def test_schema_windows_cover_schema(self):
        raw = bytes.fromhex(self.v["schema"]["raw"])
        datas = {}
        for t in self.v["frames"]:
            if t["cmd"] == gp.GET_SCHEMA | gp.REPLY and t["fields"]["status"] == gp.OK:
                datas[t["fields"]["offset"]] = bytes.fromhex(t["fields"]["data"])
        self.assertEqual(datas[0], raw[:64])
        last = max(k for k in datas if k < len(raw))
        self.assertEqual(datas[last], raw[last:])


if __name__ == "__main__":
    unittest.main()
