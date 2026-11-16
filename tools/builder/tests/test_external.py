"""External components (gemifi_external.py, core/README.md "External components"): loading from a local
folder, what an external component may be, replacing gemifi's own, the type numbers, the generated C, the
config hash, and that one device's external components don't reach another's (the dashboard's threads).
The fixture is core/test/external/ (a TCA9555 hub type, an ST7567 display platform), also built and run by
core/test test_external."""
import os
import re
import shutil
import sys
import tempfile
import textwrap
import threading
import unittest

TESTS = os.path.dirname(os.path.abspath(__file__))
BUILDER = os.path.dirname(TESTS)
REPO = os.path.dirname(os.path.dirname(BUILDER))
sys.path.insert(0, BUILDER)
import gemifi_device as gd  # noqa: E402  (first: it puts the repo root, for components/, on the path)
import gemifi_external as ge  # noqa: E402
import components  # noqa: E402
from components import api, display, hub  # noqa: E402

FIXTURE = os.path.join(REPO, "core", "test", "external")

HEAD = """\
gemifi: 2
name: ext
board_id: 0x3F01
platform: {platform}
chip: {chip}
usb: {{ vid: 0x1209, pid: 0x0002 }}
"""
ESP = {"platform": "esp32s3", "chip": "esp32-s3-devkitc-1"}

TCA = """\
i2c: { sda: GPIO8, scl: GPIO9 }
tca9555:
  - { id: exp, address: 0x21 }
buttons:
  - { id: b0, pin: { tca9555: exp, number: 0, inverted: true }, action: cc_momentary, cc: 20 }
"""
LCD = """\
i2c: { sda: GPIO8, scl: GPIO9 }
display:
  - { id: lcd, platform: st7567, address: 0x3F }
displays:
  - { id: top, display: lcd, line: 0, shows: last_message }
"""


def errors(diags):
    return [str(d) for d in diags if d.level == "error"]


def warnings(diags):
    return [str(d) for d in diags if d.level == "warning"]


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="gemifi-ext-")
        self.addCleanup(shutil.rmtree, self.dir)
        self.ext = os.path.join(self.dir, "ext")
        shutil.copytree(FIXTURE, self.ext)

    def component(self, name, init, files=None, folder=None):
        """A component folder name in folder (default self.ext): its __init__.py and other files."""
        d = os.path.join(folder or self.ext, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "__init__.py"), "w", encoding="utf-8") as f:
            f.write(textwrap.dedent(init))
        for fn, text in (files or {}).items():
            with open(os.path.join(d, fn), "w", encoding="utf-8") as f:
                f.write(text)
        return d

    def device(self, body, ext="external_components:\n  - source: ext\n", target=ESP, name="dev.yaml"):
        p = os.path.join(self.dir, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write(HEAD.format(**target) + ext + body)
        return p

    def load(self, body, **kw):
        return gd.load_device(self.device(body, **kw))

    def ok(self, body, **kw):
        dev, diags = self.load(body, **kw)
        self.assertIsNotNone(dev, "\n".join(errors(diags)))
        return dev, diags

    def fails(self, body, *want, **kw):
        dev, diags = self.load(body, **kw)
        self.assertIsNone(dev)
        text = "\n".join(errors(diags))
        for w in want:
            self.assertIn(w, text)
        return text


class Loading(Base):
    def test_hub_type_and_display_platform(self):
        dev, _ = self.ok(TCA + LCD.replace("i2c: { sda: GPIO8, scl: GPIO9 }\n", ""))
        self.assertEqual(dev.hubs[0].kind.key, "tca9555")
        self.assertEqual(dev.screens[0].type, "st7567")
        self.assertEqual(sorted(dev.registry.external), ["st7567", "tca9555"])

    def test_numbered_after_the_builtins(self):
        dev, _ = self.ok(TCA + LCD.replace("i2c: { sda: GPIO8, scl: GPIO9 }\n", ""))
        self.assertEqual((dev.hubs[0].kind.enum, dev.hubs[0].kind.num), ("GM_HUB_EXT_TCA9555", hub.BUILTIN_TYPES))
        self.assertEqual((dev.screens[0].kind.enum, dev.screens[0].kind.num),
                         ("GM_SCREEN_EXT_ST7567", display.BUILTIN_TYPES))

    def test_only_the_listed_components(self):
        dev, _ = self.ok(TCA, ext="external_components:\n  - { source: ext, components: [tca9555] }\n")
        self.assertEqual(sorted(dev.registry.external), ["tca9555"])
        self.fails(LCD, "display[0].platform", ext="external_components:\n  - { source: ext, components: [tca9555] }\n")

    def test_source_mapping_and_components_folder(self):
        os.makedirs(os.path.join(self.dir, "repo"))
        shutil.move(self.ext, os.path.join(self.dir, "repo", "components"))
        self.ok(TCA, ext="external_components:\n  - source: { type: local, path: repo }\n")

    def test_relative_to_the_file_that_names_it(self):
        """A package in a subfolder names its source relative to itself."""
        sub = os.path.join(self.dir, "pkg")
        os.makedirs(sub)
        shutil.move(self.ext, os.path.join(sub, "ext"))
        with open(os.path.join(sub, "parts.yaml"), "w", encoding="utf-8") as f:
            f.write("external_components:\n  - source: ext\n")
        self.ok(TCA, ext="packages:\n  parts: pkg/parts.yaml\n")

    def test_builtin_untouched(self):
        """Another device, and gemifi's own tables, don't see the external types."""
        self.ok(TCA)
        self.assertNotIn("tca9555", gd.hub_tables(components.builtin()).HUB_DEFS)
        self.assertNotIn("tca9555", gd.REG.HUB_DEFS)
        dev, diags = gd.load_device(self.device(TCA, ext="", name="plain.yaml"))
        self.assertIsNone(dev)
        self.assertTrue(any("tca9555" in e and "unknown key" in e for e in errors(diags)), errors(diags))

    def test_threads_see_their_own(self):
        """Dashboard threads: a device with external components and one without, at the same time."""
        ext_path, plain_path = self.device(TCA), self.device(TCA, ext="", name="plain.yaml")
        results = {}

        def run(key, path):
            for _ in range(5):
                dev, _ = gd.load_device(path)
                results.setdefault(key, []).append(dev is not None)
        ts = [threading.Thread(target=run, args=("ext", ext_path)), threading.Thread(target=run, args=("plain", plain_path))]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        self.assertEqual(results, {"ext": [True] * 5, "plain": [False] * 5})


class Problems(Base):
    def test_no_folder(self):
        self.fails(TCA, "external_components[0].source", "no such folder",
                   ext="external_components:\n  - source: nowhere\n")

    def test_not_in_the_folder(self):
        self.fails(TCA, "external_components[0].components[0]", "'nope' isn't a component",
                   ext="external_components:\n  - { source: ext, components: [nope] }\n")

    def test_git_not_yet(self):
        for ext in ("external_components:\n  - source: github://someone/parts@v1\n",
                    "external_components:\n  - source: { type: git, url: x }\n",
                    "external_components:\n  - { source: ext, refresh: 1d }\n"):
            with self.subTest(ext=ext):
                self.fails(TCA, "git sources aren't supported yet", ext=ext)

    def test_import_error_says_where(self):
        self.component("broken", "GEMIFI_API = 1\nraise ValueError('no chip here')\n")
        text = self.fails(TCA, "component 'broken' doesn't load: ValueError: no chip here",
                          ext="external_components:\n  - { source: ext, components: [broken] }\n")
        self.assertRegex(text, r"broken/__init__\.py:2\)")

    def test_syntax_error(self):
        self.component("broken", "GEMIFI_API = (\n")
        self.fails(TCA, "component 'broken' doesn't load", ext="external_components:\n  - { source: ext, components: [broken] }\n")

    def test_api_version(self):
        self.component("old", "GEMIFI_API = 0\nfrom components.hub import I2CExpander\n"
                              "HUB_TYPES = (I2CExpander('oldx', '', 0, 8, [0x20], '0x20', c_ops='o'),)\n")
        self.fails(TCA, f"was written for component API 0; this builder has {api.API_VERSION}",
                   ext="external_components:\n  - { source: ext, components: [old] }\n")
        self.component("none", "from components.hub import I2CExpander\n"
                               "HUB_TYPES = (I2CExpander('nonex', '', 0, 8, [0x20], '0x20', c_ops='o'),)\n")
        self.fails(TCA, "declares no GEMIFI_API", ext="external_components:\n  - { source: ext, components: [none] }\n")

    def test_only_platforms(self):
        self.component("ctl", "GEMIFI_API = 1\ndef in_device(dev):\n    return True\n")
        self.fails(TCA, "is a control (in_device)", "lists no HUB_TYPES or DISPLAY_PLATFORMS",
                   ext="external_components:\n  - { source: ext, components: [ctl] }\n")

    def test_targets(self):
        """PLATFORMS: a component for the STM32F1 only, used on an ESP32-S3; checked where it is used."""
        init = os.path.join(self.ext, "st7567", "__init__.py")
        with open(init, encoding="utf-8") as f:
            text = re.sub(r"^PLATFORMS = \(.*\)", 'PLATFORMS = ("stm32f1",)', f.read(), flags=re.M)
        with open(init, "w", encoding="utf-8") as f:
            f.write(text)
        self.fails(LCD, "component 'st7567'", "doesn't build on esp32s3 (its PLATFORMS: stm32f1)")
        self.ok(TCA)            # loaded but not used: fine

    def test_key_clashes(self):
        self.component("dup", "GEMIFI_API = 1\nfrom components.hub import I2CExpander\n"
                              "HUB_TYPES = (I2CExpander('mcp23017', '', 0, 8, [0x20], '0x20', c_ops='o'),)\n")
        self.fails(TCA, "hub type 'mcp23017' is also component 'mcp23017''s",
                   ext="external_components:\n  - { source: ext, components: [dup] }\n")
        self.component("top", "GEMIFI_API = 1\nfrom components.hub import I2CExpander\n"
                              "HUB_TYPES = (I2CExpander('buttons', '', 0, 8, [0x20], '0x20', c_ops='o'),)\n")
        self.fails(TCA, "hub type 'buttons' would be a top-level list",
                   ext="external_components:\n  - { source: ext, components: [top] }\n")

    def test_same_component_twice(self):
        other = os.path.join(self.dir, "other")
        shutil.copytree(FIXTURE, other)
        self.fails(TCA, "component 'tca9555' is in two sources",
                   ext="external_components:\n  - source: ext\n  - source: other\n")


class Replacing(Base):
    PCF = """\
i2c: { sda: GPIO8, scl: GPIO9 }
pcf8574:
  - { id: io, address: 0x20 }
buttons:
  - { id: b0, pin: { pcf8574: io, number: 0, inverted: true }, action: cc_momentary, cc: 20 }
"""

    def replacement(self, name, init):
        return self.component(name, init, {f"{name}.c": "/* replacement */\n"})

    def test_replaces_with_a_warning_and_keeps_the_number(self):
        self.replacement("pcf8574", """\
            from components.hub import I2CExpander
            GEMIFI_API = 1
            HUB_TYPES = (I2CExpander("pcf8574", "", 0, 8, list(range(0x20, 0x28)), "0x20-0x27", c_ops="my_pcf_ops"),)
            """)
        dev, diags = self.ok(self.PCF, ext="external_components:\n  - { source: ext, components: [pcf8574] }\n")
        self.assertTrue(any("replaces gemifi's own (components/pcf8574)" in w for w in warnings(diags)), warnings(diags))
        self.assertEqual((dev.hubs[0].kind.enum, dev.hubs[0].kind.num), ("GM_HUB_PCF8574", 5))
        files = {**gd.generate(dev), **gd.component_lists(dev, "dev.yaml")}
        self.assertIn("[GM_HUB_PCF8574] = &my_pcf_ops,", files["gm_components.h"])
        self.assertNotIn("GM_HUB_N_ALL", files["gm_components.h"])      # no new numbers
        mk = files["gm_components.mk"]
        self.assertIn(os.path.join(self.ext, "pcf8574", "pcf8574.c"), mk)
        self.assertNotIn("components/pcf8574/pcf8574.c", mk)

    def test_core_handled_types_cant_be_replaced(self):
        for name in ("cd74hc4067", "decoder", "switch_matrix", "tm1628", "xy_pad"):
            with self.subTest(name):
                self.replacement(name, "GEMIFI_API = 1\nfrom components.hub import I2CExpander\n"
                                       f"HUB_TYPES = (I2CExpander('{name}', '', 0, 8, [0x20], '0x20', c_ops='o'),)\n")
                self.fails(TCA, f"can't replace gemifi's own components/{name}", "core/src/vpin.c handles",
                           ext=f"external_components:\n  - {{ source: ext, components: [{name}] }}\n")

    def test_bases_and_controls_cant_be_replaced(self):
        for name, why in (("display", "a base"), ("buttons", "isn't a platform"),
                          ("led_strip", "isn't a platform")):
            with self.subTest(name):
                self.replacement(name, """\
                    from components.display import ScreenType
                    GEMIFI_API = 1
                    class S(ScreenType):
                        platform = "zzz"
                        c_header, c_config, c_ops = "z.h", ("z_t", "z"), "z_ops"
                    DISPLAY_PLATFORMS = (S(),)
                    """)
                self.fails(TCA, f"can't replace gemifi's own components/{name}", why,
                           ext=f"external_components:\n  - {{ source: ext, components: [{name}] }}\n")

    def test_every_replaceable_builtin(self):
        """The platform components a replacement may take the place of: those vpin.c only reaches through
        gm_hub_ops_t (and the display platforms)."""
        own = components.builtin().modules()
        ok = sorted(n for n in own if ge._not_replaceable(n, own) is None)
        self.assertEqual(ok, ["ht16k33", "maudio_pads", "max7219", "mcp23017", "pcf8574", "sn74hc165", "sn74hc595",
                              "ssd1306", "tm1637", "ws2812"])


class Generated(Base):
    def test_c_side(self):
        dev, _ = self.ok(TCA + LCD.replace("i2c: { sda: GPIO8, scl: GPIO9 }\n", ""))
        files = {**gd.generate(dev), **gd.component_lists(dev, "dev.yaml")}
        h = files["gm_components.h"]
        self.assertIn(f"#define GM_HUB_EXT_TCA9555 {hub.BUILTIN_TYPES}", h)
        self.assertIn(f"#define GM_HUB_N_ALL {hub.BUILTIN_TYPES + 1}", h)
        self.assertIn(f"#define GM_SCREEN_EXT_ST7567 {display.BUILTIN_TYPES}", h)
        self.assertIn('#include "st7567.h"', h)
        c = files["gm_device_config.c"]
        self.assertIn(f"[GM_HUB_N_TYPES == {hub.BUILTIN_TYPES} ? 1 : -1]", c)
        self.assertIn(f"[GM_SCREEN_N_TYPES == {display.BUILTIN_TYPES} ? 1 : -1]", c)
        mk, cm = files["gm_components.mk"], files["gm_components.cmake"]
        for text in (mk, cm):
            self.assertIn(os.path.join(self.ext, "tca9555", "tca9555.c"), text)
            self.assertIn(os.path.join(self.ext, "st7567"), text)       # its include folder
            self.assertNotIn("GM_ROOT)/" + self.ext, text)
            self.assertNotIn("GM_ROOT}/" + self.ext, text)

    def test_builtin_counts_match_the_headers(self):
        """hub.BUILTIN_TYPES / display.BUILTIN_TYPES are where the C enums end."""
        with open(os.path.join(REPO, "core", "include", "gm_device.h"), encoding="utf-8") as f:
            m = re.search(r"GM_HUB_N_TYPES = (\d+)", f.read())
        self.assertEqual(int(m.group(1)), hub.BUILTIN_TYPES)
        with open(os.path.join(REPO, "components", "display", "gm_screen.h"), encoding="utf-8") as f:
            enum = re.search(r"enum \{[^}]*GM_SCREEN_N_TYPES", f.read()).group(0)
        last = max(int(x) for x in re.findall(r"= (\d+)", enum))
        self.assertEqual(last + 1, display.BUILTIN_TYPES)

    def test_config_hash_covers_the_files(self):
        dev, _ = self.ok(TCA)
        before = dev.config_hash
        with open(os.path.join(self.ext, "tca9555", "tca9555.c"), "a", encoding="utf-8") as f:
            f.write("/* changed */\n")
        dev2, _ = self.ok(TCA)
        self.assertNotEqual(before, dev2.config_hash)
        # the same files, the same hash
        dev3, _ = self.ok(TCA)
        self.assertEqual(dev2.config_hash, dev3.config_hash)


class Dashboard(Base):
    """The dashboard's view of a file agrees with its build: the config hash it lists, and whether the build
    is stale, both count the external components' files."""

    def test_listed_hash_is_the_built_one(self):
        import dashboard
        import gemifi_yaml as gy
        path = self.device(TCA)
        dev, _ = gd.load_device(path)
        res = gy.resolve_file(path)
        self.assertEqual(gy.config_hash(res.data, ge.data_digest(res.data, path)), dev.config_hash)
        self.assertNotEqual(gy.config_hash(res.data), dev.config_hash)     # the YAML alone isn't
        self.assertTrue(hasattr(dashboard, "ge"))

    def test_stale_when_a_driver_changes(self):
        import gemifi_build as gb
        path = self.device(TCA)
        before = gb.source_hash(path)
        with open(os.path.join(self.ext, "tca9555", "tca9555.c"), "a", encoding="utf-8") as f:
            f.write("/* changed */\n")
        self.assertNotEqual(gb.source_hash(path), before)


if __name__ == "__main__":
    unittest.main()
