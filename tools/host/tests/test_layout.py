"""Layout and raw readings (sysex.md 4.7, 4.8): the codec, and the `layout` / `monitor` CLI commands against the
simulated app (tests/sim_device.py)."""
import contextlib
import io
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
from sim_device import SimDevice, SimPort  # noqa: E402
from test_config import SCHEMA  # noqa: E402

L, R = gp.LayoutItem, gp.RawMsg
LAYOUT = gp.Layout(76, 20, [L(gp.EV_ANALOG, 0, 2, 1, 1, 4, 4), L(gp.EV_BUTTON, 2, 0, 7, 2, 3, 2),
                            L(gp.EV_KEY, 0, gp.SHAPE_KEYS, 0, 6, 76, 14, n_keys=32, first_note=48)])
RAW = [R(gp.EV_BUTTON, 0, [0, 0, 1, 0, 0, 0, 0]), R(gp.EV_KEY, 0, [1, 0, 0, 0, 0, 0, 0, 1], rows=2, cols=4),
       R(gp.EV_ANALOG, 0, [1234, gp.RAW_NONE])]


class FakeBackend:
    def __init__(self, dev):
        self.port = SimPort(dev)

    def ports(self):
        return ["DIY box (gemifi)"]

    def open(self, name):
        return self.port


def run_cli(dev, *argv):
    a = gemifi.build_parser().parse_args(list(argv))
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            rc = gemifi.COMMANDS[a.cmd](a, FakeBackend(dev))
        except gemifi.GemifiError as e:
            return 1, str(e)
    return rc, out.getvalue()


def make_dev(**kw):
    return SimDevice(role=gp.ROLE_APP, schema=SCHEMA, **kw)


class TestCodec(unittest.TestCase):
    def test_layout_round_trip_and_skip(self):
        blob = gp.build_layout(LAYOUT)
        self.assertEqual(gp.parse_layout(blob), LAYOUT)
        # an item from a later version: longer, unknown shape; the next item still parses
        later = bytearray(blob)
        later[3] += 1
        later += bytes([9, 1, 5, 40, 30, 2, 3, 2, 0xAA, 0xBB])
        items = gp.parse_layout(bytes(later)).items
        self.assertEqual((items[-1].shape, items[-1].extra), (40, b"\xaa\xbb"))
        with self.assertRaises(gp.ProtocolError):
            gp.parse_layout(blob[:-1])
        with self.assertRaises(gp.ProtocolError):
            gp.parse_layout(b"\x02" + blob[1:])

    def test_key_rects(self):
        keys = gp.key_rects(LAYOUT.items[2])
        self.assertEqual(len(keys), 32)
        self.assertEqual(sum(1 for k in keys if not k[1]), 19)
        self.assertEqual(keys[0], (0, False, 0.0, 6, 4.0, 14))
        self.assertEqual(keys[1][:2], (1, True))                 # C#3: centred on the C/D line, at x 4
        self.assertAlmostEqual(keys[1][2] + keys[1][4] / 2, 4.0)

    def test_raw_round_trip(self):
        for m in RAW + [R(gp.EV_ENCODER, 3, [0, 16383]), R(gp.EV_PAD, 0, [3000])]:
            self.assertEqual(gp.parse_raw(gp.build_raw(m)), m)
        self.assertIsNone(gp.parse_raw(b"\x09\x00\x00\x00"))     # a kind this tool doesn't know
        with self.assertRaises(gp.ProtocolError):
            gp.parse_raw(b"\x03\x00\x00\x02\x01")


class TestCli(unittest.TestCase):
    def test_hello_lists_caps(self):
        rc, out = run_cli(make_dev(layout=LAYOUT, raw=RAW), "hello")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"caps +0x030e config log event layout monitor")

    def test_layout(self):
        rc, out = run_cli(make_dev(layout=LAYOUT), "layout")
        self.assertEqual(rc, 0, out)
        lines = out.splitlines()
        self.assertEqual(lines[0], "DIY box (gemifi): 76 x 20 units, 3 items")
        self.assertRegex(lines[1], r"analog 0 +knob +at +1,1 +4 x 4")
        self.assertRegex(lines[3], r"keys 0 +keys .* 76 x 14  32 keys from C3 \(48\)")
        rc, out = run_cli(make_dev(layout=LAYOUT), "layout", "--json")
        j = json.loads(out)
        self.assertEqual((j["width"], len(j["items"]), j["items"][2]["n_keys"]), (76, 3, 32))
        rc, msg = run_cli(make_dev(), "layout")
        self.assertEqual(rc, 1)
        self.assertIn("no layout", msg)

    def test_monitor(self):
        dev = make_dev(raw=RAW)
        rc, out = run_cli(dev, "monitor", "--seconds", "0.3")
        self.assertEqual(rc, 0, out)
        self.assertIn("buttons  0..6: pressed 2", out)
        self.assertIn("matrix   rows 0..1: #... ...#", out)
        self.assertIn("analog   0..1: 1234 -", out)
        self.assertEqual(dev.monitor_kinds[0], 31)
        self.assertEqual(dev.monitor_kinds[-1], 0)               # stopped on the way out
        dev = make_dev(raw=RAW)
        rc, out = run_cli(dev, "monitor", "analog", "--seconds", "0.2")
        self.assertEqual((dev.monitor_kinds[0], out.strip()), (gp.MON_ANALOG, "analog   0..1: 1234 -"))
        rc, msg = run_cli(make_dev(), "monitor", "--seconds", "0")
        self.assertEqual(rc, 1)
        self.assertIn("no raw readings", msg)


if __name__ == "__main__":
    unittest.main()
