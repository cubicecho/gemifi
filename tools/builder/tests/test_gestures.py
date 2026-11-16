"""Button gestures (components/buttons/README.md "Gestures"): the YAML's checks, the generated C, the params
and the schema. core/test/test_gestures.c checks how they behave."""
import os
import shutil
import tempfile
import unittest

from tests import test_builder as tb

gd = tb.gd

BASE = tb.MINIMAL_NUC + """\
  - { id: b, pin: PA2, action: note, note: 60 }
  - { id: c, pin: PA3, action: cc_momentary, cc: 3 }
"""


def with_gesture(line, base=BASE, extra=""):
    """base with button a's line ending in `, <line> }`, then extra."""
    return base.replace("action: cc_momentary, cc: 1 }", f"action: cc_momentary, cc: 1, {line} }}") + extra


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestHold(unittest.TestCase):
    errs = tb.TestValidation.errs
    assertError = tb.TestValidation.assertError
    assertClean = tb.TestValidation.assertClean

    def gen(self, text):
        dev, diags = tb.load(text)
        self.assertIsNotNone(dev, [str(d) for d in diags])
        files = gd.generate(dev, "t.yaml")
        return dev, files["gm_device_config.h"], files["gm_device_config.c"]

    def warns(self, text):
        return [d.msg for d in self.errs(text, level="warning")]

    def test_none_is_left_out(self):
        """No gesture: no gesture define, table or param, so the image stays as it was."""
        dev, h, c = self.gen(BASE)
        self.assertNotIn("GM_BTN_HOLD", h)
        self.assertNotIn("gm_btn_gestures", c)
        self.assertEqual([n for n, _i, _s, count in gd.PARAMS if n.startswith("button.") and n[7:] in
                          ("hold_time",) and count(dev)], [])

    def test_hold_generates(self):
        """hold: takes the button's action and number unless it gives its own; a string names an action."""
        dev, h, c = self.gen(with_gesture("hold: { cc: 21 }", BASE.replace("note: 60 }", "note: 60, hold: panic }"),
                                          "button_settings: { hold_time: 350ms }\n"))
        self.assertIn("#define GM_BTN_HOLD 0x3ull /* hold: a, b */", h)
        self.assertIn("#define GM_BTN_HOLD_TIME_DEFAULT 350 ", h)
        self.assertIn("{ { 1, 21 } }, /* a */", c)                      # cc_momentary, CC 21
        self.assertIn(f"{{ {{ {gd.ACTION_IDS['panic']}, 0 }} }}, /* b */", c)   # panic takes no number
        self.assertIn("{ { GM_BTN_NO_ACT, 0 } }, /* c */", c)
        self.assertIn("#define GM_MSG_PANIC 1", h)                     # a gesture's message kind is compiled in
        self.assertEqual([n for n, _i, s, count in gd.PARAMS if n == "button.hold_time" and count(dev)],
                         ["button.hold_time"])
        self.assertEqual(dev.btn_set["hold_time"], 350)

    def test_hold_checks(self):
        self.assertError(with_gesture("hold: {}"), "give the action it runs")
        self.assertError(with_gesture("hold: cc_toggle"), "a gesture can't run action cc_toggle")
        self.assertError(with_gesture("hold: bend_up"), "a gesture can't run action bend_up")
        self.assertError(with_gesture("hold: { daw: play }"), "unknown key")
        self.assertError(with_gesture("hold: nope"), "unknown action 'nope'")
        self.assertError(BASE.replace("cc: 3 }", "cc: 3, hold: panic }").replace("cc_momentary, cc: 3",
                                                                                 "cc_ramp, cc: 3"),
                         "works for as long as the button is held")
        self.assertError(with_gesture("hold: { action: preset_select, preset: 2 }"), "presets are off")

    def test_settings(self):
        self.assertError(with_gesture("hold: panic", extra="button_settings: { hold_time: 50ms }\n"),
                         r"50ms is out of range \(100ms\.\.5000ms\)")
        self.assertError(with_gesture("hold: panic", extra="button_settings: { hold_time: 300 }\n"), "needs a unit")
        self.assertError(with_gesture("hold: panic", extra="button_settings: { tap: 300ms }\n"), "unknown key")
        self.assertTrue(any("no button has hold:" in w for w in self.warns(BASE + "button_settings: { hold_time: 300ms }\n")))

    def test_modes_and_presets(self):
        """A mode's toggle, a modifier and a with: button have no gestures; a preset can't make a button
        with one a held action."""
        modes = BASE.replace(", action: note, note: 60 }", " }") + "modes:\n"
        self.assertError(modes.replace("{ id: b, pin: PA2 }", "{ id: b, pin: PA2, hold: panic }") +
                         "  - { id: m, toggle: b, layer: { c: { cc: 9 } } }\n", "switches mode 'm'")
        self.assertError(with_gesture("hold: panic", modes) + "  - { id: m, hold: a, layer: { c: { cc: 9 } } }\n",
                         "is held for mode 'm'")
        self.assertError(with_gesture("hold: panic", modes) +
                         "  - { id: m, toggle: { button: c, with: a }, layer: { c: { cc: 9 } } }\n",
                         "with: button of mode 'm'")
        presets = with_gesture("hold: panic", extra="presets:\n  slots: 2\n  list:\n    - { name: A }\n"
                                                    "    - { name: B, buttons: { a: { action: cc_ramp } } }\n")
        self.assertError(presets, "has hold:, so its presses wait")
        self.assertError(with_gesture("hold: panic", extra="presets:\n  slots: 2\n  list:\n    - { name: A }\n"
                                                           "    - { name: B, buttons: { a: { hold: preset_next } } }\n"),
                         "the same in every preset")


@unittest.skipUnless(tb.GCC and gd.yaml, "needs host gcc and PyYAML")
class TestGesturesCompile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    compile_with_core = tb.TestGenerateCompiles.compile_with_core
    gcc = tb.TestGenerateCompiles.gcc

    def test_compiles(self):
        """Each gesture alone, and all of them, compile warning-free with the core."""
        for tag, text in (("hold", with_gesture("hold: { cc: 21 }")),):
            with self.subTest(tag):
                dev, diags = tb.load(text)
                self.assertIsNotNone(dev, [str(d) for d in diags])
                self.compile_with_core(tb.gen_dir(dev, self.tmp, tag))


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestGestureSchema(unittest.TestCase):
    def test_schema(self):
        import gemifi_schema as gs
        s = gs.device_schema()
        button = s["properties"]["buttons"]["anyOf"][0]["items"]["properties"]
        self.assertIn("hold", button)
        self.assertNotIn("cc_toggle", button["hold"]["anyOf"][0]["anyOf"][0]["enum"])
        settings = s["properties"]["button_settings"]["anyOf"][0]["properties"]
        self.assertEqual(sorted(settings), sorted(gd.buttons.BUTTON_SETTINGS))


if __name__ == "__main__":
    unittest.main()
