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
        for define in ("GM_BTN_HOLD", "GM_BTN_DOUBLE"):
            self.assertNotIn(define, h)
        self.assertNotIn("gm_btn_gestures", c)
        self.assertEqual([n for n, _i, _s, count in gd.PARAMS if n.startswith("button.") and n[7:] in
                          gd.buttons.BUTTON_SETTINGS and count(dev)], [])

    def test_hold_generates(self):
        """hold: takes the button's action and number unless it gives its own; a string names an action."""
        dev, h, c = self.gen(with_gesture("hold: { cc: 21 }", BASE.replace("note: 60 }", "note: 60, hold: panic }"),
                                          "button_settings: { hold_time: 350ms }\n"))
        self.assertIn("#define GM_BTN_HOLD 0x3ull /* hold: a, b */", h)
        self.assertIn("#define GM_BTN_HOLD_TIME_DEFAULT 350 ", h)
        none = "{ GM_BTN_NO_ACT, 0 }"
        self.assertIn(f"{{ {{ 1, 21 }}, {none} }}, /* a */", c)              # cc_momentary, CC 21
        self.assertIn(f"{{ {{ {gd.ACTION_IDS['panic']}, 0 }}, {none} }}, /* b */", c)   # panic takes no number
        self.assertIn(f"{{ {none}, {none} }}, /* c */", c)
        self.assertNotIn("GM_BTN_DOUBLE", h)
        self.assertIn("#define GM_MSG_PANIC 1", h)                     # a gesture's message kind is compiled in
        self.assertEqual([n for n, _i, s, count in gd.PARAMS if n == "button.hold_time" and count(dev)],
                         ["button.hold_time"])
        self.assertEqual(dev.btn_set["hold_time"], 350)

    def test_double_tap_generates(self):
        """double_tap: like hold:, with its own mask, table column, param and default."""
        dev, h, c = self.gen(with_gesture("double_tap: { cc: 22 }, hold: { action: note, note: 70 }",
                                          extra="button_settings: { tap_time: 250ms }\n"))
        self.assertIn("#define GM_BTN_DOUBLE 0x1ull /* double_tap: a */", h)
        self.assertIn("#define GM_BTN_TAP_TIME_DEFAULT 250 ", h)
        self.assertIn("#define GM_BTN_HOLD 0x1ull /* hold: a */", h)
        self.assertIn("{ { 4, 70 }, { 1, 22 } }, /* a */", c)
        self.assertEqual(sorted(n for n, _i, s, count in gd.PARAMS if n in ("button.tap_time", "button.hold_time")
                                and count(dev) and s == gd.GLOBAL), ["button.hold_time", "button.tap_time"])
        dev, h, _c = self.gen(with_gesture("double_tap: program_next"))
        self.assertNotIn("GM_BTN_HOLD", h)
        self.assertNotIn("button.hold_time", [n for n, _i, _s, count in gd.PARAMS if count(dev)])
        self.assertError(with_gesture("double_tap: { action: cc_ramp }"), "a gesture can't run action cc_ramp")
        self.assertError(with_gesture("double_tap: panic", extra="button_settings: { tap_time: 2s }\n"),
                         "out of range")

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


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestCombos(unittest.TestCase):
    errs = tb.TestValidation.errs
    assertError = tb.TestValidation.assertError
    assertClean = tb.TestValidation.assertClean
    gen = TestHold.gen
    warns = TestHold.warns

    def test_generates(self):
        dev, h, c = self.gen(BASE + "combos:\n  - { buttons: [a, c], action: panic }\n"
                                    "  - { buttons: [a, b, c], action: note, note: 70 }\n"
                                    "button_settings: { combo_time: 30ms }\n")
        self.assertIn("#define GM_N_COMBOS 2 ", h)
        self.assertIn("#define GM_COMBO_BUTTONS 0x7ull /* in a combo: a, b, c */", h)
        self.assertIn("#define GM_N_COMBO_BUTTONS 3", h)
        self.assertIn("#define GM_BTN_COMBO_TIME_DEFAULT 30 ", h)
        self.assertIn(f"{{ 0x5ull, {{ {gd.ACTION_IDS['panic']}, 0 }} }}, /* a + c: panic */", c)
        self.assertIn("{ 0x7ull, { 4, 70 } }, /* a + b + c: note */", c)
        self.assertNotIn("gm_btn_gestures", c)          # no hold: or double_tap:
        self.assertNotIn("GM_BTN_HOLD", h)
        self.assertEqual([n for n, _i, _s, count in gd.PARAMS if n.startswith("button.") and n[7:] in
                          gd.buttons.BUTTON_SETTINGS and count(dev)], ["button.combo_time"])
        self.assertNotIn("GM_N_COMBOS", self.gen(BASE)[1])

    def test_checks(self):
        combo = lambda body: BASE + "combos:\n  - " + body + "\n"   # noqa: E731
        self.assertClean(combo("{ buttons: [a, b], action: panic }"))
        self.assertError(combo("{ buttons: [a], action: panic }"), "needs 2 or more buttons")
        self.assertError(combo("{ buttons: [a, a], action: panic }"), "is listed twice")
        self.assertError(combo("{ buttons: [a, nope], action: panic }"), "no button with id 'nope'")
        self.assertError(combo("{ buttons: [a, b] }"), "needs action:")
        self.assertError(combo("{ buttons: [a, b], action: cc_toggle, cc: 3 }"), "a combo can't run action cc_toggle")
        self.assertError(combo("{ buttons: [a, b], action: cc_momentary }"), "needs cc:")
        self.assertError(combo("{ buttons: [a, b], action: panic, daw: play }"), "unknown key")
        self.assertError(BASE + "combos:\n  - { buttons: [a, b], action: panic }\n  - { buttons: [b, a], action: panic }\n",
                         r"combos\[0\] has the same buttons")
        self.assertError(combo("{ buttons: [a, c], action: panic }").replace("cc_momentary, cc: 3", "bend_up"),
                         "has action bend_up, which works for as long as the button is held")
        modes = BASE + "modes:\n  - { id: m, hold: c, layer: { b: { note: 61 } } }\n"
        self.assertError(modes + "combos:\n  - { buttons: [a, c], action: panic }\n", "is held for mode 'm'")
        self.assertError(modes + "combos:\n  - { buttons: [a, b_m], action: panic }\n", "is a layer twin")
        self.assertClean(modes + "combos:\n  - { buttons: [a, b], action: panic }\n")    # a twin's base may be
        self.assertTrue(any("there are no combos:" in w for w in self.warns(BASE + "button_settings: { combo_time: 30ms }\n")))
        presets = combo("{ buttons: [a, b], action: panic }") + ("presets:\n  slots: 2\n  list:\n    - { name: A }\n"
                                                                 "    - { name: B, buttons: { a: { action: cc_ramp } } }\n")
        self.assertError(presets, "has a combo, so its presses wait")


ONE_SHOT = BASE + "  - { id: fn, pin: PA4 }\nmodes:\n"


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestOneShot(unittest.TestCase):
    errs = tb.TestValidation.errs
    assertError = tb.TestValidation.assertError
    assertClean = tb.TestValidation.assertClean
    gen = TestHold.gen

    def test_generates(self):
        dev, h, c = self.gen(ONE_SHOT + "  - { id: f, one_shot: fn, layer: { a: { cc: 9 } } }\n")
        self.assertIn("#define GM_MODE_ONE_SHOT 1 ", h)
        # the mode's row: toggle fn (3), hold GM_HOLD_ONE_SHOT (2)
        self.assertRegex(c, r"\{ 3, GM_MODE_NONE, 0, -1, 0, -1, -1, 0, 0, 2, 0, 0, 0, 0, 0x00, \"f\", 0 \}, /\* f \*/")
        self.assertIn("#define GM_LAYERS 1 ", h)
        self.assertNotIn("GM_MODE_ONE_SHOT", self.gen(ONE_SHOT + "  - { id: f, hold: fn, layer: { a: { cc: 9 } } }\n")[1])
        self.assertClean(ONE_SHOT + "  - { id: f, one_shot: { button: fn }, layer: { a: { cc: 9 } } }\n")

    def test_checks(self):
        self.assertError(ONE_SHOT + "  - { id: f, one_shot: fn, buttons: { a: channel_up } }\n",
                         "a one-shot mode has a layer: and nothing else")
        self.assertError(ONE_SHOT + "  - { id: f, one_shot: fn }\n", "a one-shot mode has a layer:")
        self.assertError(ONE_SHOT + "  - { id: f, one_shot: a, layer: { b: { note: 61 } } }\n",
                         "arms this mode, so its own action would never run")
        self.assertError(ONE_SHOT + "  - { id: f, one_shot: fn, toggle: fn, layer: { a: { cc: 9 } } }\n",
                         "one of toggle:, hold:, one_shot: or parent:")
        self.assertError(ONE_SHOT + "  - { id: f, one_shot: { button: fn, with: a }, layer: { a: { cc: 9 } } }\n",
                         "unknown key")
        self.assertError(ONE_SHOT + "  - { id: f, one_shot: fn, layer: { fn: { cc: 9 } } }\n", "enters a mode")
        self.assertError(ONE_SHOT.replace("{ id: fn, pin: PA4 }", "{ id: fn, pin: PA4, hold: panic }") +
                         "  - { id: f, one_shot: fn, layer: { a: { cc: 9 } } }\n", "switches mode 'f'")
        self.assertError(ONE_SHOT + "  - { id: f, one_shot: fn, layer: { a: { cc: 9 } } }\n"
                                    "combos:\n  - { buttons: [a, fn], action: panic }\n", "switches mode 'f'")


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
        for tag, text in (("hold", with_gesture("hold: { cc: 21 }")),
                          ("double_tap", with_gesture("double_tap: { cc: 21 }")),
                          ("both", with_gesture("double_tap: { cc: 21 }, hold: panic")),
                          ("combos", BASE + "combos:\n  - { buttons: [a, b], action: panic }\n"),
                          ("all", with_gesture("double_tap: { cc: 21 }, hold: panic",
                                               extra="combos:\n  - { buttons: [a, b], action: panic }\n")),
                          ("one_shot", ONE_SHOT.replace("{ id: fn, pin: PA4 }", "{ id: fn, pin: PA4, led: l }") +
                           "  - { id: f, one_shot: fn, layer: { a: { cc: 9 } } }\nleds:\n  - { id: l, pin: PB0 }\n")):
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
        for g in ("hold", "double_tap"):
            self.assertIn(g, button)
        self.assertNotIn("cc_toggle", button["hold"]["anyOf"][0]["anyOf"][0]["enum"])
        settings = s["properties"]["button_settings"]["anyOf"][0]["properties"]
        self.assertEqual(sorted(settings), sorted(gd.buttons.BUTTON_SETTINGS))
        mode = s["properties"]["modes"]["anyOf"][0]["items"]["properties"]
        self.assertIn("one_shot", mode)
        combo = s["properties"]["combos"]["anyOf"][0]["items"]["properties"]
        self.assertEqual(sorted(combo), sorted(gd.buttons.COMBO_KEYS))


if __name__ == "__main__":
    unittest.main()
