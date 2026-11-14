"""The hub types the builder knows, and that they match the core's GM_HUB_* values."""
import os
import re
import sys
import unittest
from types import SimpleNamespace

TESTS = os.path.dirname(os.path.abspath(__file__))
BUILDER = os.path.dirname(TESTS)
REPO = os.path.dirname(os.path.dirname(BUILDER))
sys.path.insert(0, BUILDER)
import gemifi_device as gd  # noqa: E402  (puts the repo root, and so components/, on the path)
import components  # noqa: E402
from components.hub import HubType  # noqa: E402

HUB_KEYS = {
    "sn74hc595", "sn74hc165", "cd74hc4051", "cd74hc4067", "mcp23017", "pcf8574", "decoder", "switch_matrix",
    "tm1628", "maudio_pad_leds", "xy_pad", "ws2812", "max7219", "tm1637", "ht16k33",
}


def core_hub_enum():
    """GM_HUB_* name -> value, from core/include/gm_device.h."""
    with open(os.path.join(REPO, "core", "include", "gm_device.h")) as f:
        return {m[1]: int(m[2]) for m in re.finditer(r"\b(GM_HUB_[A-Z0-9_]+) = (\d+),", f.read())}


class HubTypes(unittest.TestCase):
    def test_every_hub_type_is_known(self):
        self.assertEqual(set(gd.HUB_DEFS), HUB_KEYS)

    def test_enums_match_the_core(self):
        core = core_hub_enum()
        for key, t in gd.HUB_DEFS.items():
            with self.subTest(key):
                self.assertEqual(core[t.enum], t.num)

    def test_every_hub_type_is_a_top_level_key(self):
        for key in gd.HUB_DEFS:
            with self.subTest(key):
                self.assertIn(key, gd.TOP_KEYS)


class Discovery(unittest.TestCase):
    def test_finds_the_component_folders(self):
        self.assertIn("hub", components.names())
        self.assertEqual(components.names(), sorted(components.names()))

    def test_component_hub_types_are_hub_types(self):
        for t in components.hub_types():
            with self.subTest(t.key):
                self.assertIsInstance(t, HubType)
                self.assertIs(gd.HUB_DEFS[t.key], t)

    def test_hub_types_are_in_enum_order(self):
        self.assertEqual(list(gd.HUB_DEFS), [t.key for t in sorted(gd.HUB_DEFS.values(), key=lambda t: (t.num, t.key))])


KEYSTATION = os.path.join(REPO, "devices", "keystation-mini32-mk3.yaml")


class SourceLists(unittest.TestCase):
    def test_the_hub_base_is_always_built(self):
        self.assertIn("hub", components.used([]))

    def test_written_with_the_config(self):
        dev, _ = gd.load_device(KEYSTATION)
        lists = gd.component_lists(dev, "devices/keystation-mini32-mk3.yaml")
        self.assertEqual(set(lists), {"gm_components.mk", "gm_components.cmake"})
        self.assertRegex(lists["gm_components.mk"], r"(?m)^GM_COMPONENTS := .*\bhub\b")
        self.assertRegex(lists["gm_components.cmake"], r"(?m)^set\(GM_COMPONENTS .*\bhub\b")
        for line in lists["gm_components.mk"].splitlines():
            if line.startswith(("GM_COMPONENT_SRCS", "GM_COMPONENT_INCS")):
                for path in line.split(":=")[1].split():
                    self.assertTrue(path.startswith("$(GM_ROOT)/components/"), path)


class Header(unittest.TestCase):
    def test_no_component_hub_types(self):
        dev, _ = gd.load_device(KEYSTATION)
        h = gd.generate(dev)["gm_components.h"]
        for macro in ("GM_HUB_CONFIG_MEMBERS", "GM_HUB_STATE_MEMBERS", "GM_HUB_OPS_EXTERNS", "GM_HUB_OPS"):
            self.assertRegex(h, rf"(?m)^#define {macro}$")

    def test_a_component_hub_type(self):
        class Strip(HubType):
            key, enum, num = "strip", "GM_HUB_STRIP", 20
            c_header, c_config, c_state, c_ops = "strip.h", ("gm_hub_strip_t", "st"), ("gm_strip_state_t", "st"), "gm_strip_ops"
        dev = SimpleNamespace(hubs=[SimpleNamespace(kind=Strip()), SimpleNamespace(kind=Strip())], screens=[])
        h = gd.component_header(dev, "x.yaml")["gm_components.h"]
        self.assertEqual(h.count('#include "strip.h"'), 1)
        self.assertIn("gm_hub_strip_t st;", h)
        self.assertIn("gm_strip_state_t st;", h)
        self.assertIn("extern const gm_hub_ops_t gm_strip_ops;", h)
        self.assertIn("[GM_HUB_STRIP] = &gm_strip_ops,", h)


if __name__ == "__main__":
    unittest.main()
