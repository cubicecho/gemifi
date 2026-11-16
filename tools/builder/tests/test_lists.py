"""Action lists (components/buttons/README.md "Action lists"): the YAML's checks, the generated C, the
per-preset lists and the schema. core/test/test_lists.c checks how they behave."""
import shutil
import tempfile
import unittest

from tests import test_builder as tb

gd = tb.gd

BASE = tb.MINIMAL_NUC + """\
  - { id: b, pin: PA2, action: note, note: 60 }
"""
LIST = "actions: [{ program: 5, channel: 2 }, { cc: 7, value: 100 }, { cc: 64 }, { note: 36, channel: 10 }]"
PRESETS = "presets:\n  slots: 3\n  list:\n    - { name: A }\n"


def with_list(text=LIST, base=BASE, extra=""):
    """base with button a's action replaced by `text`, then extra."""
    return base.replace("action: cc_momentary, cc: 1 }", f"{text} }}") + extra


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestActionLists(unittest.TestCase):
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
        """No list: no define, table or action option, so the image stays as it was."""
        _dev, h, c = self.gen(BASE)
        self.assertNotIn("GM_BTN_LISTS", h)
        self.assertNotIn("gm_btn_items", c)

    def test_generates(self):
        """A list is action 23 with button.number its index; the items are a flash table, in order."""
        dev, h, c = self.gen(with_list())
        self.assertEqual(dev.buttons[0].action, gd.ACTION_IDS["actions"])
        self.assertIn("#define GM_BTN_LISTS 1 ", h)
        self.assertIn("#define GM_N_BTN_ITEMS 4 ", h)
        self.assertIn("#define GM_MSG_PROGRAM 1", h)            # an item's message kind is compiled in
        self.assertIn("const uint16_t gm_btn_lists[GM_BTN_LISTS + 1] = { 0, 4 };", c)
        for item in ("{ GM_BTN_NO_VALUE, 17, 5, 2 }, /* list 0[0]: program 5 channel 2 */",
                     "{ 100, 1, 7, 0 }, /* list 0[1]: cc 7 = 100 */",
                     "{ GM_BTN_NO_VALUE, 1, 64, 0 }, /* list 0[2]: cc 64 */",
                     "{ GM_BTN_NO_VALUE, 4, 36, 10 }, /* list 0[3]: note 36 channel 10 */"):
            self.assertIn(item, c)
        self.assertIn("gm_button_action_defaults[GM_N_BUTTONS] = { 23, 4 }", c)
        self.assertIn("gm_button_number_defaults[GM_N_BUTTONS] = { 0, 60 }", c)

    def test_items(self):
        """An item is an action's name or a mapping; action: may be left out where its number key says it;
        bank: goes with a program, value: with a CC."""
        _dev, _h, c = self.gen(with_list("actions: [{ action: note, note: 40 }, panic, { program: 3, bank: 130 }, "
                                         "{ mmc: play }]"))
        for item in ("{ GM_BTN_NO_VALUE, 4, 40, 0 }, /* list 0[0]: note 40 */",
                     "{ GM_BTN_NO_VALUE, 18, 0, 0 }, /* list 0[1]: panic */",
                     "{ 130, 17, 3, 0 }, /* list 0[2]: program 3 bank 130 */",
                     "{ GM_BTN_NO_VALUE, 19, 2, 0 }, /* list 0[3]: mmc play */"):
            self.assertIn(item, c)
        self.assertError(with_list("actions: [panic, { action: note, note: 40 }]"),
                         r"comes after the panic in buttons\[0\]\.actions\[0\]")
        self.assertClean(with_list("actions: [{ note: 40 }, panic, { program: 3 }, { cc: 7, value: 1 }]"))
        self.assertError(with_list("actions: [{ cc: 7, note: 3 }]"), "give the item's message: one of")
        self.assertError(with_list("actions: [{ channel: 2 }]"), "give the item's message: one of")
        self.assertError(with_list("actions: [{ action: cc_momentary }]"), "needs cc:")
        self.assertError(with_list("actions: [{ action: panic, cc: 3 }]"), "takes no a CC or note")
        self.assertError(with_list("actions: [{ note: 3, value: 9 }]"), "value: goes with a CC")
        self.assertError(with_list("actions: [{ cc: 3, bank: 9 }]"), "bank: goes with a program")
        self.assertError(with_list("actions: [{ cc: 3, value: 200 }]"), "out of range")
        self.assertError(with_list("actions: [{ cc: 3, channel: 17 }]"), "out of range")
        self.assertError(with_list("actions: [{ cc: 3, nrpn: 9 }]"), "unknown key")
        self.assertError(with_list("actions: [{ actions: [panic] }]"), "unknown key")
        self.assertError(with_list("actions: []"), "give the messages it sends")
        self.assertError(with_list("actions: [" + ", ".join(["panic"] * 33) + "]"), "at most 32")

    def test_refused_kinds(self):
        """Stateful and held actions, and preset switches, can't be items; each says why."""
        for act, why in (("cc_toggle", "on/off state"), ("cc_ramp", "held"), ("bend_up", "held"),
                         ("octave_up", "steps a setting"), ("transpose_down", "steps a setting"),
                         ("program_next", "steps a setting"), ("preset_next", "preset switch"), ("none", "nothing")):
            with self.subTest(act):
                self.assertError(with_list(f"actions: [{{ action: {act} }}]", extra=PRESETS),
                                 f"can't send action {act}: .*{why}")
        self.assertError(with_list("actions: [{ action: mcu }]"), "can't send action mcu")

    def test_button_keys(self):
        """actions: replaces action: and the number keys; daw:, bank: and repeat: mean nothing on a list
        button; channel:, min: and max: are the items' defaults."""
        self.assertError(with_list(LIST + ", action: note"), "give action: or actions:, not both")
        self.assertError(with_list(LIST + ", cc: 3"), "cc: goes in an item of actions:")
        self.assertError(with_list(LIST + ", daw: play"), "a DAW role names one control's")
        self.assertError(with_list(LIST + ", bank: 3"), "bank: goes in the list's program items")
        self.assertError(with_list(LIST + ", repeat: true"), "drop repeat:")
        self.assertError(with_list("action: actions"), "goes in actions:")
        _dev, h, c = self.gen(with_list(LIST + ", channel: 3, max: 100"))
        self.assertIn("#define GM_BTN_CTL 1", h)

    def test_same_list_once(self):
        """Two buttons with the same list share it."""
        text = with_list(base=BASE.replace("action: note, note: 60 }", LIST + " }"))
        dev, h, _c = self.gen(text)
        self.assertIn("#define GM_BTN_LISTS 1 ", h)
        self.assertEqual([b.number for b in dev.buttons], [0, 0])

    def test_gestures_combos_twins(self):
        """hold:, double_tap:, a combo and a layer twin take actions: too; an override of a list button
        without action: or actions: is the same list, and a number key there an error."""
        dev, h, c = self.gen(with_list("action: cc_momentary, cc: 1, hold: { actions: [{ cc: 2 }, { note: 9 }] }"))
        self.assertIn("{ { 23, 0 }, { GM_BTN_NO_ACT, 0 } }, /* a */", c)
        _dev, _h, c = self.gen(BASE + "combos:\n  - { buttons: [a, b], actions: [{ cc: 43 }, panic] }\n")
        self.assertIn("{ 0x3ull, { 23, 0 } }, /* a + b: actions */", c)
        modes = with_list(base=BASE + "  - { id: fn, pin: PA3 }\n") + "modes:\n"
        dev, _h, c = self.gen(modes + "  - { id: m, hold: fn, layer: { b: { actions: [{ cc: 12 }] } } }\n")
        twin = dev.buttons[-1]
        self.assertEqual((twin.action_name, twin.layer), ("actions", 1))
        self.assertError(modes + "  - { id: m, hold: fn, layer: { a: { cc: 12 } } }\n",
                         r"sends an action list \(actions:\), so cc: has no action to go with")
        self.assertError(modes + "  - { id: m, hold: fn, layer: { b: { actions: [{ cc: 1 }], daw: play } } }\n",
                         "a DAW role names one control's")
        dev, _h, _c = self.gen(modes + "  - { id: m, hold: fn, layer: { a: { name: Twin } } }\n")
        self.assertEqual((dev.buttons[-1].action_name, dev.buttons[-1].number), ("actions", 0))
        self.assertError(with_list(LIST + ", hold: { cc: 21 }"), "so cc: has no action to go with")
        self.assertError(with_list(LIST + ", double_tap: { cc: 21 }"), "so cc: has no action to go with")
        self.assertError(modes.replace("{ id: fn, pin: PA3 }", f"{{ id: fn, pin: PA3, {LIST} }}") +
                         "  - { id: m, toggle: fn, layer: { b: { cc: 9 } } }\n", "remove its actions:")

    def test_presets(self):
        """A preset gives a button another list as button.number (and button.action when the button had
        none), or one action again."""
        text = with_list(extra=PRESETS + "    - { name: B, buttons: { a: { actions: [{ cc: 90 }] }, "
                                         "b: { actions: [{ program: 1 }] } } }\n"
                                         "    - { name: C, buttons: { a: { action: note, note: 61 } } }\n")
        dev, _h, c = self.gen(text)
        self.assertEqual(len(dev.btn_lists), 3)
        self.assertIn("{ 10, 0, 1 }, /* button.number[a] = 1 */", c)
        self.assertIn("{ 9, 1, 23 }, /* button.action[b] = actions */", c)
        self.assertIn("{ 10, 1, 2 }, /* button.number[b] = 2 */", c)
        self.assertIn("{ 9, 0, 4 }, /* button.action[a] = note */", c)
        self.assertFalse([w for w in self.warns(text) if "the same as in preset 1" in w])
        self.assertError(with_list(extra=PRESETS + "    - { name: B, buttons: { a: { cc: 5 } } }\n"),
                         "so cc: has no action to go with")
        self.assertError(with_list(extra=PRESETS + "    - { name: B, buttons: { a: { actions: [cc_toggle] } } }\n"),
                         "can't send action cc_toggle")

    def test_too_many_lists(self):
        """button.number (0..127) picks the list: 128 different ones at most (here from 63 presets giving
        three buttons a list each)."""
        base = BASE + "  - { id: c, pin: PA3, action: note, note: 61 }\n"
        presets = "".join(f"    - {{ name: P{k}, buttons: {{ a: {{ actions: [{{ cc: {k} }}] }}, "
                          f"b: {{ actions: [{{ note: {k} }}] }}, c: {{ actions: [{{ program: {k} }}] }} }} }}\n"
                          for k in range(63))
        text = with_list(base=base, extra="presets:\n  slots: 64\n  list:\n    - { name: A }\n" + presets)
        self.assertError(text, "more than 128 different action lists")

    def test_not_an_action_name(self):
        """The list action (23) has an id but is no name for action:: a list is given as actions:."""
        self.assertEqual(gd.ACTION_IDS["actions"], 23)
        self.assertNotIn("actions", gd.ACTIONS)


@unittest.skipUnless(tb.GCC and gd.yaml, "needs host gcc and PyYAML")
class TestActionListsCompile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    compile_with_core = tb.TestGenerateCompiles.compile_with_core
    gcc = tb.TestGenerateCompiles.gcc

    def test_compiles(self):
        """A list alone (no other message kind on), with every kind, and on a hold: compile warning-free."""
        for tag, text in (("cc_note", with_list("actions: [{ cc: 7, value: 1 }, { note: 3, channel: 2 }]")),
                          ("kinds", with_list("actions: [{ note: 1 }, { sysex: s }, panic, { program: 2, bank: 3 }, "
                                              "{ mmc: stop }]", extra="midi_messages:\n  sysex:\n"
                                                                     "    - { id: s, data: \"F0 41 v F7\" }\n")),
                          ("hold", with_list("action: cc_momentary, cc: 1, hold: { actions: [{ cc: 2 }] }"))):
            with self.subTest(tag):
                dev, diags = tb.load(text)
                self.assertIsNotNone(dev, [str(d) for d in diags])
                self.compile_with_core(tb.gen_dir(dev, self.tmp, tag))


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestActionListSchema(unittest.TestCase):
    def test_schema(self):
        import gemifi_schema as gs
        s = gs.device_schema()
        button = s["properties"]["buttons"]["anyOf"][0]["items"]["properties"]
        item = button["actions"]["items"]["anyOf"]
        self.assertEqual(item[0]["anyOf"][0]["enum"], list(gd.buttons.ITEM_ACTIONS))
        self.assertEqual(sorted(item[1]["properties"]), sorted(gd.buttons.ITEM_KEYS))
        self.assertNotIn("actions", button["action"]["anyOf"][0]["enum"])
        self.assertIn("actions", button["hold"]["anyOf"][1]["properties"])
        combo = s["properties"]["combos"]["anyOf"][0]["items"]["properties"]
        self.assertIn("actions", combo)
        preset_button = (s["properties"]["presets"]["anyOf"][0]["properties"]["list"]["items"]["properties"]
                         ["buttons"]["additionalProperties"]["properties"])
        self.assertIn("actions", preset_button)


if __name__ == "__main__":
    unittest.main()
