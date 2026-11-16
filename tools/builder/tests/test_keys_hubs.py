"""Hub types and display platforms whose keys are data (components/keys.py), as test_keys.py does for the
controls: the validator takes exactly what each key's type says (the edges of its range in, one past them
out), so the schema and the docs table, which read the same data, say what the builder does."""
import json
import os
import sys
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(TESTS))
import gemifi_device as gd  # noqa: E402  (puts the repo root, and so components/, on the path)

HEAD = ("gemifi: 2\nname: x\nboard_id: 0x3F01\nplatform: stm32f1\nchip: STM32F103RC\n"
        "usb: { vid: 0x1209, pid: 0x0002 }\n")
# I2C components: ESP only for now (components/i2c/README.md)
ESP = ("gemifi: 2\nname: x\nboard_id: 0x3F02\nplatform: esp32\nchip: some-board\nmidi: { transport: serial }\n"
       "i2c: { sda: GPIO21, scl: GPIO22 }\n")


def errors(yaml, head=HEAD):
    """The errors of a device (head + yaml): [(path, message)]."""
    _, diags = gd.load_device("x.yaml", head + yaml)
    return [(d.path, d.msg) for d in diags if d.level == "error"]


def section(name, item, many=True):
    return f"{name}:\n  - {json.dumps(item)}\n" if many else f"{name}: {json.dumps(item)}\n"


class EdgesAgree(unittest.TestCase):
    def edges(self, sec, base, path, keys, context=None, extra="", skip=(), many=True, head=HEAD, own=()):
        """Each key of keys (a Keys) whose type has edges: those in pass, those past fail at path.key.
        context: key -> the keys the item needs for that key's edges, or a function of the value giving
        them; extra: more YAML the device needs; skip: keys checked elsewhere; head: the device's start;
        own: keys whose edges in only need no error of their own (a rule between keys may still refuse them)."""
        context = context or {}
        checked = 0
        for k in keys:
            if k.name in skip:
                continue
            for value, ok in k.type.bad_values():
                with self.subTest(key=k.name, value=value):
                    item = json.loads(json.dumps(base))
                    more = context.get(k.name, {})
                    item.update(more(value) if callable(more) else more)
                    item[k.name] = value
                    errs = errors(section(sec, item, many) + extra, head)
                    if ok and k.name in own:
                        self.assertNotIn(f"{path}.{k.name}", [p for p, _ in errs], (item, errs))
                    elif ok:
                        self.assertEqual(errs, [], item)
                    else:
                        self.assertIn(f"{path}.{k.name}", [p for p, _ in errs], (item, errs))
                    checked += 1
        return checked

    def hub(self, typ, base, n, **kw):
        t = gd.HUB_DEFS[typ]
        sec = t.domain or t.key
        if t.domain:
            base = {**base, "platform": typ}
        if t.i2c:
            kw.setdefault("head", ESP)
        self.assertGreaterEqual(self.edges(sec, base, f"{sec}[0]", t.spec.without("platform"), **kw), n)

    def test_sn74hc595(self):
        self.hub("sn74hc595", {"id": "h", "data_pin": "PB15", "clock_pin": "PB13", "latch_pin": "PB14"}, 4)

    def test_sn74hc165(self):
        self.hub("sn74hc165", {"id": "h", "data_pin": "PB12", "clock_pin": "PB13", "load_pin": "PB14"}, 4)

    def test_i2c_expanders(self):
        self.hub("mcp23017", {"id": "h"}, 4)
        self.hub("pcf8574", {"id": "h"}, 4)

    def test_muxes(self):
        sel = {"pin_s0": "PB6", "pin_s1": "PB7", "pin_s2": "PB8", "common_pin": "PA0"}
        self.hub("cd74hc4051", {"id": "h", **sel}, 4)
        self.hub("cd74hc4067", {"id": "h", **sel, "pin_s3": "PB9"}, 4)

    def test_tm1628(self):
        self.hub("tm1628", {"id": "h", "clk_pin": "PB6", "dio_pin": "PB7", "stb_pin": "PB8"}, 12)

    def test_max7219(self):
        self.hub("max7219", {"id": "h", "clk_pin": "PB6", "din_pin": "PB7", "cs_pin": "PB8"}, 12)

    def test_ht16k33(self):
        self.hub("ht16k33", {"id": "h"}, 8)

    def test_xy_pad(self):
        pins = {"x_plus_pin": "PB1", "x_minus_pin": "PC5", "y_plus_pin": "PC4", "y_minus_pin": "PB2"}
        # touch_min 4095 and touch_max 0 are in range, but touch_min must be below touch_max
        self.hub("xy_pad", {"id": "h", **pins}, 8, own=("touch_min", "touch_max"))

    def test_strips(self):
        self.hub("ws2812", {"id": "h", "pin": "PA7", "count": 4}, 8)
        self.hub("maudio_pads", {"id": "h", "tx_pin": "PA2"}, 8)

    def test_switch_matrix(self):
        self.hub("switch_matrix", {"id": "h", "rows": ["PB6", "PB7"], "columns": ["PB8", "PB9"]}, 8)


    def test_ssd1306(self):
        t = gd.DISPLAY_PLATFORMS["ssd1306"]
        # width and height are checked together too: each edge with a size the panel has
        n = self.edges("display", {"id": "d", "platform": "ssd1306"}, "display[0]", t.spec.without("platform"),
                       head=ESP, context={"width": lambda w: {"height": 48 if w == 64 else 64},
                                          "height": lambda ht: {"width": 96 if ht == 16 else 128}})
        self.assertGreaterEqual(n, 20)


class Specs(unittest.TestCase):
    def specs(self):
        """(name, its spec, its YAML key names) of every hub type, display platform and i2c: bus."""
        return [*((typ, t.spec, gd.HUB_KEYS[typ]) for typ, t in gd.HUB_DEFS.items()),
                *((p, t.spec, t.keys) for p, t in gd.DISPLAY_PLATFORMS.items()),
                ("i2c", gd.i2c_buses.BUS, gd.i2c_buses.BUS_KEYS)]

    def test_every_hub_and_screen_has_one(self):
        for name, spec, _ in self.specs():
            self.assertIsNotNone(spec, f"{name}: give it a spec (components/keys.py)")

    def test_the_validator_lists_the_spec_keys(self):
        for name, spec, keys in self.specs():
            self.assertEqual(list(keys), spec.names, name)

    def test_every_key_says_what_it_does(self):
        for name, spec, _ in self.specs():
            for k in spec:
                self.assertTrue(k.doc.strip(), f"{name}.{k.name} has no doc=")
                for s in (k.sub_keys() or ()):
                    self.assertTrue(s.doc.strip(), f"{name}.{k.name}.{s.name} has no doc=")


if __name__ == "__main__":
    unittest.main()
