"""Components whose keys are data (components/keys.py): the validator takes exactly what each key's type says
(the edges of its range in, one past them out), so the schema and the docs table, which read the same data,
say what the builder does."""
import json
import os
import sys
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(TESTS))
import gemifi_device as gd  # noqa: E402  (puts the repo root, and so components/, on the path)
import gemifi_schema as gs  # noqa: E402
from components import encoders, tm1637  # noqa: E402
from components.keys import Map, OneOf  # noqa: E402

HEAD = ("gemifi: 2\nname: x\nboard_id: 0x3F01\nplatform: stm32f1\nchip: STM32F103RC\n"
        "usb: { vid: 0x1209, pid: 0x0002 }\n")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(TESTS)))


def errors(section, item):
    """The errors of a device whose `section` list is [item]: [(path, message)]."""
    _, diags = gd.load_device("x.yaml", HEAD + f"{section}:\n  - {json.dumps(item)}\n")
    return [(d.path, d.msg) for d in diags if d.level == "error"]


class EdgesAgree(unittest.TestCase):
    def edges(self, section, base, path, keys, context=None, nest=None):
        """Each key of keys (a Keys) whose type has edges: those in pass, those past fail at path.key.
        context: key -> keys the item needs for that key's edges; nest: put the key in base[nest]."""
        context = context or {}
        checked = 0
        for k in keys:
            for value, ok in k.type.bad_values():
                with self.subTest(key=k.name, value=value):
                    item = json.loads(json.dumps(base))
                    extra = dict(context.get(k.name, {}))
                    drop = extra.pop("-", ())
                    if nest:
                        item[nest] = {**item.get(nest, {}), **extra, k.name: value}
                    else:
                        item.update(extra)
                        item[k.name] = value
                    for d in drop:
                        item.pop(d, None)
                    errs = errors(section, item)
                    at = f"{path}.{nest}.{k.name}" if nest else f"{path}.{k.name}"
                    if ok:
                        self.assertEqual(errs, [], item)
                    else:
                        self.assertIn(at, [p for p, _ in errs], (item, errs))
                    checked += 1
        return checked

    def test_encoder(self):
        base = {"id": "e", "pin_a": "PB6", "pin_b": "PB7", "cc": 20}
        n = self.edges("encoders", base, "encoders[0]", encoders.ENCODER, context={
            "nrpn": {"mode": "nrpn", "-": ["cc"]}, "rpn": {"mode": "rpn", "-": ["cc"]},
            "min": {"mode": "nrpn14", "nrpn": 1, "max": 5, "-": ["cc"]},
            "max": {"mode": "nrpn14", "nrpn": 1, "min": 5, "-": ["cc"]}})
        self.assertGreater(n, 20)

    def test_encoder_acceleration(self):
        base = {"id": "e", "pin_a": "PB6", "pin_b": "PB7", "cc": 20}
        n = self.edges("encoders", base, "encoders[0]", encoders.ACCEL, nest="acceleration",
                       context={"slow": {"fast": "1ms"}, "fast": {"slow": "1000ms"}})
        self.assertGreater(n, 10)

    def test_tm1637(self):
        base = {"id": "d", "platform": "tm1637", "clk_pin": "PB6", "dio_pin": "PB7"}
        self.assertGreater(self.edges("led_driver", base, "led_driver[0]", tm1637.SPEC.without("platform")), 6)


class KeyData(unittest.TestCase):
    def test_checked_in_keys_are_current(self):
        with open(os.path.join(REPO, gs.KEYS_FILE), encoding="utf-8") as f:
            self.assertEqual(f.read(), gs.keys_text(), f"run `python3 tools/builder/gemifi_build.py keys -o "
                                                       f"{gs.KEYS_FILE}`")

    def test_every_key_says_what_it_does(self):
        def walk(keys, where):
            for k in keys:
                self.assertTrue(k.doc.strip(), f"{where}.{k.name} has no doc=")
                sub = k.type.keys if isinstance(k.type, Map) else k.type.keys() if isinstance(k.type, OneOf) else None
                if sub:
                    walk(sub, f"{where}.{k.name}")
        for name, sections in gs.component_keys().items():
            self.assertTrue(sections, name)
        walk(encoders.ENCODER, "encoders")
        walk(encoders.ENCODER_SETTINGS, "encoder_settings")
        walk(tm1637.SPEC, "tm1637")

    def test_the_validator_lists_the_spec_keys(self):
        self.assertEqual(encoders.ENCODER_KEYS, encoders.ENCODER.names)
        self.assertEqual(list(gd.HUB_KEYS["tm1637"]), tm1637.SPEC.names)


if __name__ == "__main__":
    unittest.main()
