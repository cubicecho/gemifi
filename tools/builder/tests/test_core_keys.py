"""The device's own top-level keys as data (gemifi_device.py CORE_AREAS, the docs' Core pages): every top-level
key is in an area or is a component's, each area has its page, and the validator takes exactly what each
key's type says (the edges of its range in, one past them out)."""
import json
import os
import sys
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(TESTS))
import gemifi_device as gd  # noqa: E402  (puts the repo root, and so components/, on the path)
import gemifi_schema as gs  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(TESTS)))
# A device with every required top-level key; a test replaces or adds one.
BASE = {"gemifi": 2, "name": "x", "board_id": 0x3F01, "platform": "stm32f1", "chip": "STM32F103RC",
        "usb": {"vid": 0x1209, "pid": 0x0002}}


def errors(top, more=""):
    """The errors of BASE with the top-level keys `top` (None: drop it), plus the YAML `more`: [(path, msg)]."""
    d = {**BASE, **top}
    text = "".join(f"{k}: {json.dumps(x)}\n" for k, x in d.items() if x is not None) + more
    _, diags = gd.load_device("x.yaml", text)
    return [(x.path, x.msg) for x in diags if x.level == "error"]


class Coverage(unittest.TestCase):
    def test_every_top_level_key_is_somewhere(self):
        """Each top-level key is in one Core area, or is a component's section, a hub list or display:."""
        mine = {s.yaml_key for s in gs.key_sections()} | set(gd.HUB_LISTS) | {"display"}
        seen = {}
        for area, keys in gd.CORE_AREAS.items():
            for k in keys.names:
                self.assertNotIn(k, seen, f"{k}: in {seen.get(k)} and {area}")
                seen[k] = area
        for k in gd.TOP_KEYS:
            self.assertTrue(k in seen or k in mine, f"{k}: in no Core area (gemifi_device.py CORE_AREAS)")
        self.assertFalse(set(seen) & mine, "a component's key in a Core area too")

    def test_every_area_has_its_page(self):
        for area in gd.CORE_AREAS:
            p = os.path.join(REPO, "core", "yaml", f"{area}.md")
            with self.subTest(area):
                self.assertTrue(os.path.exists(p), p)
                with open(p, encoding="utf-8") as f:
                    self.assertIn("\n<!-- keys -->\n", f.read())


class EdgesAgree(unittest.TestCase):
    def edges(self, keys, context=None, nest=None, more="", base=None):
        """Each key of keys (a Keys) whose type has edges: those in pass (no error at the key's path), those
        past fail there. context: key -> other top-level keys its edges need; nest: the keys are of the
        top-level key `nest` (a mapping: base, else {})."""
        context = context or {}
        n = 0
        for k in keys:
            for value, ok in k.type.bad_values():
                with self.subTest(key=k.name, value=value):
                    top = dict(context.get(k.name, {}))
                    if nest:
                        top[nest] = {**(base or {}), **top.pop(nest, {}), k.name: value}
                        at = f"{nest}.{k.name}"
                    else:
                        top[k.name] = value
                        at = k.name
                    errs = errors(top, more)
                    if ok:
                        self.assertNotIn(at, [p for p, _ in errs], (top, errs))
                    else:
                        self.assertIn(at, [p for p, _ in errs], (top, errs))
                    n += 1
        return n

    def test_device(self):
        self.assertGreater(self.edges(gd.DEVICE_KEYS.without("platform")), 3)

    # a device with a velocity keybed, a button, an encoder, an analog input and a pad: every preset key exists
    CONTROLS = ("key_matrix: { type: velocity, rows: [PB0, PB1], columns: [PA0, PA1] }\n"
                "buttons:\n  - { id: b, pin: PB2, action: cc_momentary, cc: 20 }\n"
                "encoders:\n  - { id: e, pin_a: PB6, pin_b: PB7, cc: 20 }\n"
                "analog:\n  - { id: a, pin: PA2, cc: 7 }\n"
                "pads:\n  - { id: p, pin: PA3, note: 36 }\n")

    def deep(self, keys, at, more=""):
        """The edges of keys deeper in the file: at(key, value) -> (the top-level keys to add, the key's path)."""
        n = 0
        for k in keys:
            for value, ok in k.type.bad_values():
                with self.subTest(key=k.name, value=value):
                    top, path = at(k.name, value)
                    errs = errors(top, more)
                    (self.assertNotIn if ok else self.assertIn)(path, [p for p, _ in errs], (top, errs))
                    n += 1
        return n

    def slot_2(self, keys, at):
        """The edges of keys in preset 2 (slot 1 takes only name: and program:): at(key, value) -> (its keys,
        the key's path)."""
        def put(k, x):
            item, path = at(k, x)
            return {"presets": {"list": [{"name": "A"}, {"name": "B", **item}]}}, path
        return self.deep(keys, put, self.CONTROLS)

    def test_presets(self):
        # 64 slots fit an ESP32's NVS, not the STM32F1's config pages
        n = self.edges(gd.PRESETS_SPEC.without("list"), nest="presets",
                       context={"slots": {"platform": "esp32", "chip": "esp32", "usb": None}})
        n += self.slot_2(gd.PRESET_ITEM, lambda k, x: ({k: x}, f"presets.list[1].{k}"))
        for sec, keys in gd.PRESET_GLOBALS.items():
            n += self.slot_2(keys, lambda k, x: ({sec: {k: x}}, f"presets.list[1].{sec}.{k}"))
        for sec, cid in (("encoders", "e"), ("pads", "p")):
            keys = gd.PRESET_ITEM[sec].sub_keys().without("nrpn", "rpn")    # (they need a mode that takes them)
            n += self.slot_2(keys, lambda k, x: ({sec: {cid: {k: x}}}, f"presets.list[1].{sec}.{cid}.{k}"))
        self.assertGreater(n, 30)

    def test_displays(self):
        seg = {"led_driver": [{"id": "d", "platform": "tm1637", "clk_pin": "PB6", "dio_pin": "PB7"}]}
        item = {"id": "s", "chars": [{"led_driver": "d", "number": 0}]}
        n = self.deep(gd.DISPLAY.without(*gd.TEXT_KEYS), lambda k, x: (
            {**seg, "displays": [{**item, k: x}]}, f"displays[0].{k}"))
        # a text display on a 128 x 64 OLED (x: its room for a character is a rule of its own: 0..122 at scale 1)
        esp = {"platform": "esp32", "chip": "esp32", "usb": None, "midi": {"transport": "serial"},
               "i2c": {"sda": "GPIO21", "scl": "GPIO22"}, "display": [{"id": "oled", "platform": "ssd1306"}]}
        text = {"id": "t", "display": "oled"}
        n += self.deep(gd.DISPLAY.without(*gd.SEGMENT_KEYS, "x"), lambda k, x: (
            {**esp, "displays": [{**text, k: x}]}, f"displays[0].{k}"))
        self.assertGreater(n, 10)

if __name__ == "__main__":
    unittest.main()
