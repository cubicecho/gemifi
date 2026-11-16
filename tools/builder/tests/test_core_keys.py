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


if __name__ == "__main__":
    unittest.main()
