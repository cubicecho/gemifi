"""Host feedback (sysex.md 4.6): the GET_COMPONENT and FB_* codec, the simulated feedback app
(tests/sim_device.py), the Session's notify path, and the `components` / `fb` CLI commands."""
import contextlib
import io
import json
import os
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import gemifi  # noqa: E402
import gemifi_proto as gp  # noqa: E402
from sim_device import FEEDBACK_COMPONENTS, SimDevice, SimPort  # noqa: E402

C = gp.Component


def make_dev(**kw):
    kw.setdefault("components", True)
    return SimDevice(role=gp.ROLE_APP, **kw)


def session(dev):
    return gemifi.Session(SimPort(dev), timeout=0.005)


class TestNumbers(unittest.TestCase):
    def test_constants(self):
        self.assertEqual(gp.GET_COMPONENT, 0x11)
        self.assertEqual((gp.FB_LED, gp.FB_LED_RGB, gp.FB_LEDS, gp.FB_VALUE, gp.FB_RING, gp.FB_TEXT, gp.FB_RELEASE),
                         tuple(range(0x20, 0x27)))
        self.assertEqual(gp.CAP_FEEDBACK, 64)
        self.assertEqual(gp.KINDS, (1, 2, 3, 4, 5, 6))
        self.assertEqual((gp.KIND_BUTTON, gp.KIND_KEYS, gp.KIND_ANALOG, gp.KIND_ENCODER),
                         (gp.EV_BUTTON, gp.EV_KEY, gp.EV_ANALOG, gp.EV_ENCODER))
        self.assertEqual(gp.NONE_INDEX, 0x3FFF)
        for c in range(0x20, 0x27):
            self.assertTrue(gp.is_notification(c))
            self.assertIn(c, gp.CMD_NAMES)
        self.assertEqual(gp.CMD_NAMES[gp.GET_COMPONENT], "GET_COMPONENT")
        self.assertFalse(gp.is_notification(gp.GET_COMPONENT))
        self.assertFalse(gp.is_notification(0x40) or gp.is_notification(0x1F))
        self.assertTrue(gp.is_notification(0x3F))
        self.assertNotIn(gp.GET_COMPONENT, gp.LONG_TIMEOUT_CMDS)


class TestLstring(unittest.TestCase):
    def test_encode(self):
        self.assertEqual(gp.enc_lstring(""), b"\x00")
        self.assertEqual(gp.enc_lstring("play"), b"\x04play")
        self.assertEqual(gp.enc_lstring("x" * 31), b"\x1f" + b"x" * 31)
        for bad in ("x" * 32, "café", "tab\there", "\x7f"):
            with self.assertRaises(ValueError, msg=bad):
                gp.enc_lstring(bad)

    def test_decode(self):
        self.assertEqual(gp.dec_lstring(b"\x04playrest"), ("play", 5))
        self.assertEqual(gp.dec_lstring(b"\x1f" + b"y" * 31), ("y" * 31, 32))
        for bad in (b"", b"\x20" + b"z" * 32, b"\x05abc", b"\x02a\x80"):
            with self.assertRaises(gp.ProtocolError, msg=bad):
                gp.dec_lstring(bad)

    def test_device_cuts(self):
        """sysex.md 4.6.1: the device cuts strings over 31; the host's own encoder refuses them."""
        c = C(gp.KIND_ANALOG, 0, 1, "a" * 40, "n" * 31, "r" * 32)
        got = gp.parse_component(gp.build_component_reply(c))
        self.assertEqual((got.id, got.name, got.role), ("a" * 31, "n" * 31, "r" * 31))


class TestComponentCodec(unittest.TestCase):
    CASES = [
        C(gp.KIND_BUTTON, 1, 2, "rec", "Record", "record", led=7),
        C(gp.KIND_BUTTON, 0, 1, "shift", "Shift", "", led=gp.NONE_INDEX),
        C(gp.KIND_KEYS, 0, 1, "keys", "Keys", "", keys=61, first_note=36),
        C(gp.KIND_ANALOG, 3, 9, "fader4", "Fader 4", "track_volume"),
        C(gp.KIND_ENCODER, 0, 8, "enc1", "Encoder 1", "device_param_1", ring=11, center=300),
        C(gp.KIND_ENCODER, 1, 8, "enc2", "Encoder 2", "", ring=0, center=gp.NONE_INDEX),
        C(gp.KIND_LED, 4, 5, "rec_led", "Record LED", "", colours=2),
        C(gp.KIND_LED, 0, 5, "pad_led", "Pad", "", colours=3),
        C(gp.KIND_DISPLAY, 0, 1, "lcd", "LCD", "track_name", cols=16, rows=2, display_type=1),
        C(9, 0, 1, "future", "Future", "", extra=b"\x01\x02"),
    ]

    def test_round_trip(self):
        for c in self.CASES:
            payload = gp.build_component_reply(c)
            self.assertEqual(gp.parse_component(payload), c, c)
            f = gp.parse_frame(gp.build_frame(gp.GET_COMPONENT | gp.REPLY, 3, payload))
            self.assertTrue(gp.reply_matches(f, gp.GET_COMPONENT, 3), c)

    def test_layout(self):
        """Byte for byte, as in sysex.md 4.6.1."""
        c = C(gp.KIND_ENCODER, 2, 4, "e", "E", "pan", ring=15, center=200)
        self.assertEqual(gp.build_component_reply(c),
                         bytes([0, 4, 2, 0, 4, 0, 1]) + b"e" + b"\x01E" + b"\x03pan" + bytes([15, 0x48, 0x01]))
        b = C(gp.KIND_BUTTON, 0, 1, "", "", "")
        self.assertEqual(gp.build_component_reply(b), bytes([0, 1, 0, 0, 1, 0, 0, 0, 0, 0x7F, 0x7F]))
        k = C(gp.KIND_KEYS, 0, 1, "", "", "", keys=130, first_note=21)
        self.assertEqual(gp.build_component_reply(k)[-3:], bytes([0x02, 0x01, 21]))
        d = C(gp.KIND_DISPLAY, 0, 1, "", "", "", cols=4, rows=1, display_type=2)
        self.assertEqual(gp.build_component_reply(d)[-3:], bytes([4, 1, 2]))
        self.assertEqual(gp.build_component_reply(C(gp.KIND_ANALOG, 0, 1, "", "", "")), bytes([0, 3, 0, 0, 1, 0, 0, 0, 0]))
        self.assertEqual(gp.build_component_reply(c, gp.BAD_PARAM), bytes([gp.BAD_PARAM]))

    def test_request(self):
        self.assertEqual(gp.req_get_component(gp.KIND_LED, 200), bytes([5, 0x48, 0x01]))
        with self.assertRaises(ValueError):
            gp.req_get_component(128, 0)
        with self.assertRaises(ValueError):
            gp.req_get_component(1, 0x4000)

    def test_bad_replies(self):
        good = gp.build_component_reply(C(gp.KIND_BUTTON, 0, 1, "b", "B", "", led=1))
        for bad in (good[:-1], good[:5], bytes([gp.BAD_PARAM]), good[:6] + b"\x20" + b"x" * 32):
            with self.assertRaises(gp.ProtocolError, msg=bad):
                gp.parse_component(bad)
        self.assertEqual(gp.parse_component(good + b"\x05").led, 1)     # later fields are ignored

    def test_text_and_json(self):
        texts = [c.extras_text() for c in self.CASES]
        self.assertEqual(texts[:9], ["LED 7", "no LED", "61 keys from C2 (36)", "", "ring of 11, centre LED 300",
                                     "no ring", "bicolour", "RGB", "16x2 characters"])
        self.assertEqual(self.CASES[1].to_json()["led"], None)
        self.assertEqual(self.CASES[8].to_json()["type"], 1)


class TestNotificationCodec(unittest.TestCase):
    def test_builders(self):
        self.assertEqual(gp.fb_led(200, 127), bytes([0x48, 0x01, 127]))
        self.assertEqual(gp.fb_led_rgb(1, 127, 0, 64), bytes([1, 0, 127, 0, 64, 0]))
        self.assertEqual(gp.fb_led_rgb(1, 0, 0, 0, blink=True)[-1], 1)
        self.assertEqual(gp.fb_leds(8, [0, 1, 127]), bytes([8, 0, 3, 0, 1, 127]))
        self.assertEqual(gp.fb_value(gp.KIND_ANALOG, 2, 16383), bytes([3, 2, 0, 0x7F, 0x7F]))
        self.assertEqual(gp.fb_ring(1, gp.RING_SPREAD, 8192, center=True), bytes([1, 0, 3, 0, 0x40, 1]))
        self.assertEqual(gp.fb_text(0, 1, 2, "Hi"), bytes([0, 0, 1, 2]) + b"Hi")
        self.assertEqual(gp.fb_text(0, 0, 0, ""), bytes(4))
        self.assertEqual(gp.fb_release(), b"\x00")
        self.assertEqual(gp.fb_release(gp.REL_TEXT), b"\x03")

    def test_round_trip(self):
        cases = [
            (gp.FB_LED, gp.fb_led(3, 64), {"led": 3, "level": 64}),
            (gp.FB_LED_RGB, gp.fb_led_rgb(0x3FFE, 1, 2, 3, True), {"led": 0x3FFE, "r": 1, "g": 2, "b": 3, "blink": 1}),
            (gp.FB_LEDS, gp.fb_leds(0, [5] * 64), {"first": 0, "levels": [5] * 64}),
            (gp.FB_VALUE, gp.fb_value(gp.KIND_ENCODER, 7, 100), {"kind": 4, "index": 7, "value": 100}),
            (gp.FB_RING, gp.fb_ring(0, gp.RING_BOOST, 0), {"encoder": 0, "mode": 1, "value": 0, "center": 0}),
            (gp.FB_TEXT, gp.fb_text(1, 3, 127, "x" * 64), {"display": 1, "row": 3, "col": 127, "text": "x" * 64}),
            (gp.FB_RELEASE, gp.fb_release(gp.REL_VALUES), {"what": 2}),
        ]
        for cmd, payload, fields in cases:
            n = gp.parse_notification(cmd, payload)
            self.assertEqual((n.cmd, n.fields), (cmd, fields))
            f = gp.parse_frame(gp.build_frame(cmd, 0, payload))
            self.assertTrue(f.is_notification and not f.is_reply and not f.is_unsolicited)

    def test_range_errors(self):
        bad = [
            lambda: gp.fb_led(gp.NONE_INDEX, 0), lambda: gp.fb_led(-1, 0), lambda: gp.fb_led(0, 128),
            lambda: gp.fb_led_rgb(0, 128, 0, 0), lambda: gp.fb_led_rgb(0, 0, -1, 0),
            lambda: gp.fb_leds(0, []), lambda: gp.fb_leds(0, [0] * 65), lambda: gp.fb_leds(0, [200]),
            lambda: gp.fb_leds(0x3FFE, [0, 0]),
            lambda: gp.fb_value(gp.KIND_BUTTON, 0, 0), lambda: gp.fb_value(gp.KIND_ANALOG, 0, 16384),
            lambda: gp.fb_ring(0, 4, 0), lambda: gp.fb_ring(0, 0, 0x4000), lambda: gp.fb_ring(0x4000, 0, 0),
            lambda: gp.fb_text(0, 128, 0, ""), lambda: gp.fb_text(0, 0, 0, "x" * 65),
            lambda: gp.fb_text(0, 0, 0, "café"), lambda: gp.fb_text(0, 0, 0, "a\nb"),
            lambda: gp.fb_release(4),
        ]
        for i, fn in enumerate(bad):
            with self.assertRaises(ValueError, msg=f"case {i}"):
                fn()

    def test_malformed(self):
        for cmd, payload in ((gp.FB_LED, b"\x00\x00"), (gp.FB_LEDS, b"\x00\x00\x02\x01"), (gp.FB_LEDS, b"\x00\x00\x00"),
                             (gp.FB_VALUE, bytes([1, 0, 0, 0, 0])), (gp.FB_RING, bytes([0, 0, 4, 0, 0, 0])),
                             (gp.FB_RELEASE, b"\x04"), (gp.FB_TEXT, b"\x00\x00\x00"), (0x27, b"")):
            with self.assertRaises(gp.ProtocolError, msg=(cmd, payload)):
                gp.parse_notification(cmd, payload)


class TestSession(unittest.TestCase):
    def test_components(self):
        s = session(make_dev())
        comps = s.components()
        self.assertEqual([(c.kind, c.index, c.count, c.id) for c in comps],
                         [(1, 0, 2, "play"), (1, 1, 2, "rec"), (2, 0, 1, "keys"), (3, 0, 1, "fader1"),
                          (4, 0, 1, "enc1"), (5, 0, 3, "play_led"), (5, 1, 3, "rec_led"), (5, 2, 3, "enc1_centre")])
        self.assertIsNone(s.get_component(gp.KIND_DISPLAY, 0))     # none of a kind: BAD_PARAM for index 0
        self.assertIsNone(s.get_component(gp.KIND_BUTTON, 2))      # past the end
        self.assertEqual(s.get_component(gp.KIND_ENCODER, 0).ring, 5)

    def test_sim_reply_errors(self):
        dev = make_dev()
        r = dev.handle(gp.build_frame(gp.GET_COMPONENT, 1, b"\x01\x00"))
        self.assertEqual(gp.parse_frame(r).payload, bytes([gp.BAD_LEN]))
        r = SimDevice(role=gp.ROLE_APP).handle(gp.build_frame(gp.GET_COMPONENT, 2, gp.req_get_component(1, 0)))
        self.assertEqual(gp.parse_frame(r).payload, bytes([gp.BAD_CMD]))

    def test_notify_never_waits(self):
        dev = make_dev()
        port = SimPort(dev)
        s = gemifi.Session(port)              # the real 200 ms timeout and 5 resends
        t0 = time.monotonic()
        for i in range(50):
            s.notify(gp.FB_LED, gp.fb_led(i % 3, 127))
        self.assertLess(time.monotonic() - t0, 0.1)
        self.assertEqual(len(port.sent), 50)      # sent once each, no retries
        self.assertEqual(port.q, [])              # and nothing came back
        self.assertEqual([n.fields["led"] for n, _ in dev.notifications], [i % 3 for i in range(50)])
        self.assertEqual({seq for _, seq in dev.notifications}, {0})

    def test_notify_and_request_are_separate(self):
        s = session(make_dev())
        with self.assertRaises(ValueError):
            s.request(gp.FB_LED, gp.fb_led(0, 0))
        with self.assertRaises(ValueError):
            s.notify(gp.HELLO)

    def test_sim_drops_malformed(self):
        dev = make_dev()
        self.assertIsNone(dev.handle(gp.build_frame(gp.FB_LED, 0, b"\x00")))
        self.assertIsNone(dev.handle(gp.build_frame(0x3F, 0, b"")))
        self.assertEqual((len(dev.dropped), dev.notifications), (2, []))

    def test_old_firmware_answers_in_unsolicited_range(self):
        """Why hosts check caps bit 6 first (sysex.md 4.6.2)."""
        r = SimDevice(role=gp.ROLE_APP).handle(gp.build_frame(gp.FB_LED, 0, gp.fb_led(0, 1)))
        f = gp.parse_frame(r)
        self.assertEqual(f.cmd, gp.LOG)
        self.assertTrue(f.is_unsolicited)


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
    backend = FakeBackend(dev)
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            rc = gemifi.COMMANDS[a.cmd](a, backend)
        except gemifi.GemifiError as e:
            return 1, str(e), backend.port
    return rc, out.getvalue(), backend.port


class TestCli(unittest.TestCase):
    def test_hello_lists_cap(self):
        rc, out, _ = run_cli(make_dev(), "hello")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"caps +0x004c log event feedback")

    def test_components_table(self):
        rc, out, _ = run_cli(make_dev(), "components")
        self.assertEqual(rc, 0)
        lines = out.splitlines()
        self.assertEqual(lines[0], "DIY box (gemifi)")
        self.assertRegex(lines[1], r"kind +index +id +name +role +extras")
        self.assertEqual(len(lines), 2 + len(FEEDBACK_COMPONENTS))
        self.assertRegex(out, r"button +0 +play +Play +play +LED 0")
        self.assertRegex(out, r"keys +0 +keys +Keys +32 keys from C3 \(48\)")
        self.assertRegex(out, r"encoder +0 +enc1 +Encoder 1 +device_param_1 +ring of 5, centre LED 2")
        self.assertRegex(out, r"led +1 +rec_led +Record LED +bicolour")

    def test_components_json(self):
        rc, out, _ = run_cli(make_dev(), "components", "--json")
        j = json.loads(out)
        self.assertEqual(len(j), len(FEEDBACK_COMPONENTS))
        self.assertEqual(j[0], {"kind": 1, "kind_name": "button", "index": 0, "count": 2, "id": "play",
                                "name": "Play", "role": "play", "led": 0})

    def test_components_empty(self):
        rc, out, _ = run_cli(make_dev(components=[]), "components")
        self.assertEqual((rc, out.splitlines()[-1]), (0, "  (no components)"))

    def test_fb_led_by_id_does_not_wait(self):
        dev = make_dev()
        t0 = time.monotonic()
        rc, out, port = run_cli(dev, "fb", "led", "REC", "on")    # a button's id: its LED; any case
        self.assertLess(time.monotonic() - t0, 0.15)             # a wait for a reply would take 6 x 200 ms
        self.assertEqual(rc, 0, out)
        self.assertIn("FB_LED LED 1 = 127", out)
        self.assertEqual([(n.name, n.fields) for n, _ in dev.notifications], [("FB_LED", {"led": 1, "level": 127})])
        self.assertEqual(gp.parse_frame(port.sent[-1]).cmd, gp.FB_LED)
        self.assertEqual(port.q, [])

    def test_fb_commands(self):
        cases = [
            (("led", "2", "64"), gp.FB_LED, {"led": 2, "level": 64}),
            (("led", "rec_led", "blink"), gp.FB_LED, {"led": 1, "level": 1}),
            (("led", "enc1", "off"), gp.FB_LED, {"led": 2, "level": 0}),          # an encoder's centre LED
            (("rgb", "play_led", "127", "0", "0x10", "--blink"), gp.FB_LED_RGB,
             {"led": 0, "r": 127, "g": 0, "b": 16, "blink": 1}),
            (("leds", "0", "on", "off", "100"), gp.FB_LEDS, {"first": 0, "levels": [127, 0, 100]}),
            (("value", "analog", "fader1", "8192"), gp.FB_VALUE, {"kind": 3, "index": 0, "value": 8192}),
            (("value", "encoder", "0", "16383"), gp.FB_VALUE, {"kind": 4, "index": 0, "value": 16383}),
            (("ring", "enc1", "spread", "100", "--center"), gp.FB_RING,
             {"encoder": 0, "mode": 3, "value": 100, "center": 1}),
            (("ring", "0", "dot", "0"), gp.FB_RING, {"encoder": 0, "mode": 0, "value": 0, "center": 0}),
            (("text", "0", "1", "2", "Hello"), gp.FB_TEXT, {"display": 0, "row": 1, "col": 2, "text": "Hello"}),
            (("release",), gp.FB_RELEASE, {"what": 0}),
            (("release", "leds"), gp.FB_RELEASE, {"what": 1}),
            (("release", "text"), gp.FB_RELEASE, {"what": 3}),
        ]
        for args, cmd, fields in cases:
            dev = make_dev()
            rc, out, _ = run_cli(dev, "fb", *args)
            self.assertEqual(rc, 0, (args, out))
            self.assertEqual([(n.cmd, n.fields) for n, _ in dev.notifications], [(cmd, fields)], args)

    def test_fb_errors_send_nothing(self):
        for args, msg in ((("led", "nosuch", "1"), "no LED, button or encoder with id 'nosuch'"),
                          (("led", "0", "128"), "level 128 is outside 0..127"),
                          (("led", "0", "bright"), "level 'bright'"),
                          (("value", "analog", "enc1", "0"), "no analog with id 'enc1'"),
                          (("ring", "0", "dot", "16384"), "outside 0..16383"),
                          (("text", "lcd", "0", "0", "x"), "no display with id 'lcd'")):
            dev = make_dev()
            rc, out, port = run_cli(dev, "fb", *args)
            self.assertEqual(rc, 1, args)
            self.assertIn(msg, out, args)
            self.assertFalse(any(gp.is_notification(gp.parse_frame(m).cmd) for m in port.sent), args)
            self.assertEqual(dev.notifications, [])

    def test_refused_without_caps_bit_6(self):
        for argv in (("components",), ("fb", "led", "0", "1"), ("fb", "release")):
            dev = SimDevice(role=gp.ROLE_APP)
            rc, msg, port = run_cli(dev, *argv)
            self.assertEqual(rc, 1, argv)
            self.assertIn("no host feedback", msg)
            self.assertIn("bit 6 not set", msg)
            self.assertEqual([gp.parse_frame(m).cmd for m in port.sent], [gp.HELLO], argv)   # nothing else sent

    def test_refused_on_bootloader(self):
        rc, msg, _ = run_cli(SimDevice(), "components")
        self.assertEqual(rc, 1)
        self.assertIn("bootloader", msg)


if __name__ == "__main__":
    unittest.main()
