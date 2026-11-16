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
    def edges(self, sec, base, path, keys, context=None, extra="", skip=(), many=True, head=HEAD):
        """Each key of keys (a Keys) whose type has edges: those in pass, those past fail at path.key.
        context: key -> the keys the item needs for that key's edges, or a function of the value giving
        them; extra: more YAML the device needs; skip: keys checked elsewhere; head: the device's start."""
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
                    if ok:
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


class Specs(unittest.TestCase):
    def test_the_validator_lists_the_spec_keys(self):
        for typ, t in gd.HUB_DEFS.items():
            if t.spec is not None:
                self.assertEqual(list(gd.HUB_KEYS[typ]), t.spec.names, typ)

    def test_every_key_says_what_it_does(self):
        for typ, t in gd.HUB_DEFS.items():
            for k in t.spec or ():
                self.assertTrue(k.doc.strip(), f"{typ}.{k.name} has no doc=")


if __name__ == "__main__":
    unittest.main()
