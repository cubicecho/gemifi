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
from components import analog, buttons, encoders, pads, tm1637  # noqa: E402
from components.keys import Enum  # noqa: E402

HEAD = ("gemifi: 2\nname: x\nboard_id: 0x3F01\nplatform: stm32f1\nchip: STM32F103RC\n"
        "usb: { vid: 0x1209, pid: 0x0002 }\n")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(TESTS)))


def errors(section, item, more=""):
    """The errors of a device whose `section` is [item] (or item, when `section` ends in ':'), plus the YAML
    `more`: [(path, message)]."""
    body = f"{section[:-1]}: {json.dumps(item)}\n" if section.endswith(":") else f"{section}:\n  - {json.dumps(item)}\n"
    _, diags = gd.load_device("x.yaml", HEAD + more + body)
    return [(d.path, d.msg) for d in diags if d.level == "error"]


class EdgesAgree(unittest.TestCase):
    def edges(self, section, base, path, keys, context=None, nest=None, more=""):
        """Each key of keys (a Keys) whose type has edges: those in pass (no error at path.key), those past
        fail there. context: key -> keys the item needs for that key's edges ("-": keys to drop); nest: put
        the key in base[nest]; more: other YAML the device needs."""
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
                    errs = errors(section, item, more)
                    at = f"{path}.{nest}.{k.name}" if nest else f"{path}.{k.name}"
                    if ok:          # (rules between keys may still refuse it, at the item)
                        self.assertNotIn(at, [p for p, _ in errs], (item, errs))
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

    def test_analog(self):
        base = {"id": "a", "pin": "PA0", "cc": 7}
        n = self.edges("analog", base, "analog[0]", analog.ANALOG, context={
            "nrpn": {"mode": "nrpn", "-": ["cc"]}, "rpn": {"mode": "rpn", "-": ["cc"]},
            "min": {"mode": "nrpn14", "nrpn": 1, "max": 5, "-": ["cc"]},
            "max": {"mode": "nrpn14", "nrpn": 1, "min": 5, "-": ["cc"]},
            "adc_min": {"adc_max": 100}, "adc_max": {"adc_min": 100}})
        self.assertGreater(n, 25)

    def test_pads(self):
        n = self.edges("pads", {"id": "p", "pin": "PA0", "note": 36}, "pads[0]", pads.PAD)
        n += self.edges("pad_settings:", {}, "pad_settings", pads.PAD_SETTINGS,
                        more="pads:\n  - { id: p, pin: PA0, note: 36 }\n", context={
                            "threshold": {"release": 0, "full": 4095}, "release": {"threshold": 4094, "full": 4095},
                            "full": {"release": 0, "threshold": 1}})
        self.assertGreater(n, 25)

    # what the action keys' edges need: their action (and presets on, a 14-bit SysEx string for min/max)
    ACTION_CONTEXT = {"note": {"action": "note", "-": ["cc"]}, "preset": {"action": "preset_select", "-": ["cc"]},
                      "program": {"action": "program", "-": ["cc"]},
                      "min": {"action": "sysex", "sysex": "s", "max": 5, "-": ["cc"]},
                      "max": {"action": "sysex", "sysex": "s", "min": 5, "-": ["cc"]}}
    BUTTONS_MORE = ("presets: { slots: 2 }\n"
                    "midi_messages: { sysex: [{ id: s, data: \"F0 43 msb lsb F7\" }] }\n")

    def test_buttons(self):
        base = {"id": "b", "pin": "PB0", "action": "cc_momentary", "cc": 20}
        n = self.edges("buttons", base, "buttons[0]", buttons.BUTTON, context=self.ACTION_CONTEXT,
                       more=self.BUTTONS_MORE)
        n += self.edges("buttons", base, "buttons[0]", buttons.GESTURE.without("action"), nest="hold",
                        context={**self.ACTION_CONTEXT, "cc": {"action": "cc_momentary"}}, more=self.BUTTONS_MORE)
        self.assertGreater(n, 30)

    def test_combos(self):
        more = self.BUTTONS_MORE + "buttons:\n  - { id: a, pin: PB0 }\n  - { id: c, pin: PB1 }\n"
        n = self.edges("combos", {"buttons": ["a", "c"], "action": "cc_momentary", "cc": 20}, "combos[0]",
                       buttons.COMBO.without("min", "max"), context=self.ACTION_CONTEXT, more=more)
        self.assertGreater(n, 10)

    def test_button_settings(self):
        n = self.edges("button_settings:", {}, "button_settings", buttons.BUTTON_SETTINGS_SPEC,
                       more="buttons:\n  - { id: a, pin: PB0, action: panic }\n")
        self.assertEqual(n, 20)

    def test_tm1637(self):
        base = {"id": "d", "platform": "tm1637", "clk_pin": "PB6", "dio_pin": "PB7"}
        self.assertGreater(self.edges("led_driver", base, "led_driver[0]", tm1637.SPEC.without("platform")), 6)


class KeyData(unittest.TestCase):
    def test_checked_in_keys_are_current(self):
        with open(os.path.join(REPO, gs.KEYS_FILE), encoding="utf-8") as f:
            self.assertEqual(f.read(), gs.keys_text(), f"run `python3 tools/builder/gemifi_build.py keys -o "
                                                       f"{gs.KEYS_FILE}`")

    def test_every_key_says_what_it_does(self):
        def walk(rows, where):
            for r in rows:
                self.assertTrue(r["doc"].strip(), f"{where}.{r['key']} has no doc=")
                walk(r.get("keys", []), f"{where}.{r['key']}")
        for name, sections in gs.component_keys().items():
            self.assertTrue(sections, name)
            for s in sections:
                walk(s["keys"], f"{name}: {s['section']}")

    def test_pad_defaults_are_the_params(self):
        """pad_settings:'s documented defaults are the pad.* param defaults the C gets (PAD_DEFAULTS)."""
        for k in pads.PAD_SETTINGS:
            x = k.default
            if isinstance(k.type, Enum):
                x = k.type.values.index(x)
            elif isinstance(x, str):        # a period, in ms
                x = int(x.removesuffix("ms"))
            self.assertEqual(int(x), pads.PAD_DEFAULTS[k.name], k.name)

    def test_the_validator_lists_the_spec_keys(self):
        self.assertEqual(encoders.ENCODER_KEYS, encoders.ENCODER.names)
        self.assertEqual(list(gd.HUB_KEYS["tm1637"]), tm1637.SPEC.names)


if __name__ == "__main__":
    unittest.main()
