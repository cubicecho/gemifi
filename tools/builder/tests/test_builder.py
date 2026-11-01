import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from contextlib import redirect_stderr, redirect_stdout

TESTS = os.path.dirname(os.path.abspath(__file__))
BUILDER = os.path.dirname(TESTS)
REPO = os.path.dirname(os.path.dirname(BUILDER))
sys.path.insert(0, BUILDER)
import gemifi_build as gb  # noqa: E402
import gemifi_device as gd  # noqa: E402

CORE_INC = os.path.join(REPO, "core", "include")
CORE_SRC = os.path.join(REPO, "core", "src")
KEYSTATION = os.path.join(REPO, "devices", "keystation-mini32-mk3.yaml")
EVERYTHING = os.path.join(TESTS, "fixtures", "everything-esp32s3.yaml")
GCC = shutil.which("gcc")
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror"]

MINIMAL_NUC = """\
gemifi: 2
name: test
board_id: 0x3F01
platform: nuvoton-nuc1xx
chip: NUC120LC1BN
usb: { vid: 0x1209, pid: 0x0002 }
buttons:
  - { id: a, pin: PA1, action: cc_momentary, cc: 1 }
"""

MINIMAL_ESP = """\
gemifi: 2
name: test
board_id: 0x3F02
platform: {plat}
chip: some-board
midi: {{ transport: {tr} }}
"""


def gen_dir(dev, tmp, name="cfg"):
    d = os.path.join(tmp, name)
    gd.write_generated(dev, d, "test.yaml")
    return d


def load(text):
    return gd.load_device("t.yaml", text)


@unittest.skipUnless(GCC and gd.yaml, "needs host gcc and PyYAML")
class TestGenerateCompiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def gcc(self, *args):
        r = subprocess.run([GCC, *args], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, f"gcc {' '.join(args)}\n{r.stderr}")

    def dump(self, cfg_dir, tag):
        exe = os.path.join(self.tmp, f"dump-{tag}")
        self.gcc(*CFLAGS, "-I", CORE_INC, "-I", cfg_dir, os.path.join(TESTS, "dump_device.c"),
                 os.path.join(cfg_dir, "gm_device_config.c"), "-o", exe)
        return subprocess.run([exe], capture_output=True, text=True, check=True).stdout

    def compile_with_core(self, cfg_dir):
        """-Werror compile of the config and the core runtime against it; link settings + params."""
        objs = []
        for src in [os.path.join(cfg_dir, "gm_device_config.c")] + sorted(
                os.path.join(CORE_SRC, f) for f in os.listdir(CORE_SRC) if f.endswith(".c")):
            o = os.path.join(cfg_dir, os.path.basename(src) + ".o")
            # the generated file must be warning-free; core/ is someone else's: errors only
            flags = CFLAGS if src.startswith(cfg_dir) else ["-std=c11", "-w"]
            self.gcc(*flags, "-I", CORE_INC, "-I", cfg_dir, "-c", src, "-o", o)
            objs.append(o)
        main = os.path.join(cfg_dir, "main.c")
        with open(main, "w") as f:
            f.write("int main(void) { return 0; }\n")
        link = [o for o in objs if re.search(r"(gm_device_config|settings|params)\.c\.o$", o)]
        if len(link) == 3:
            self.gcc(*CFLAGS, main, *link, "-o", os.path.join(cfg_dir, "link-check"))

    def test_keystation(self):
        """The Keystation preset (what plain `make` and core/test build) generates, compiles with the core,
        and has the board's shape. core/test checks its behaviour."""
        dev, diags = gd.load_device(KEYSTATION)
        self.assertIsNotNone(dev, [str(d) for d in diags])
        self.assertFalse(gd.errors(diags), [str(d) for d in diags])
        mine = gen_dir(dev, self.tmp)
        out = self.dump(mine, "gen")
        self.assertIn("matrix kind=1 keys=32 first=48", out)
        for s in ("buttons=7", "leds=4", "analogs=1"):
            self.assertIn(s, out)
        self.compile_with_core(mine)

    def test_generate_keeps_unchanged_files(self):
        dev, _ = gd.load_device(KEYSTATION)
        out = os.path.join(self.tmp, "keep")
        paths = gd.write_generated(dev, out, "x.yaml")
        for p in paths:
            os.utime(p, (1, 1))
        gd.write_generated(dev, out, "x.yaml")
        self.assertEqual([os.stat(p).st_mtime for p in paths], [1, 1])

    def test_header_and_banner(self):
        dev, _ = gd.load_device(KEYSTATION)
        files = gd.generate(dev, "devices/keystation-mini32-mk3.yaml")
        for text in files.values():
            self.assertIn("do not edit; generated from devices/keystation-mini32-mk3.yaml", text)
        self.assertNotIn("GM_MIDI_TRANSPORT", files["gm_device_config.h"])   # NUC: not an ESP platform

    def test_everything_fixture(self):
        dev, diags = gd.load_device(EVERYTHING)
        self.assertIsNotNone(dev, [str(d) for d in diags if d.level == "error"])
        cfg = gen_dir(dev, self.tmp)
        self.compile_with_core(cfg)
        out = self.dump(cfg, "all")
        with open(os.path.join(cfg, "gm_device_config.h")) as f:
            h = f.read()
        self.assertIn("#define GM_MIDI_TRANSPORT GM_MIDI_USB", h)
        self.assertIn("#define GM_HAS_I2C 1", h)
        self.assertIn('name=Everything "S3" ?? test', out)
        self.assertIn("hubs=8 (ptr)", out)
        types = re.findall(r" hub \d+ type=(\d+) chain=(\d+) n_sel=(\d+) addr=0x(\w+)", out)
        self.assertEqual(types, [("1", "2", "0", "00"), ("1", "1", "0", "00"), ("2", "1", "0", "00"),
                                 ("2", "1", "0", "00"), ("3", "0", "3", "00"), ("3", "0", "4", "00"),
                                 ("4", "0", "0", "21"), ("5", "0", "0", "38")])
        # unused hub pins are GM_NO_PIN (hub 255), never MCU pin 0
        hub0 = out.split(" hub 0 ")[1].split(" hub 1 ")[0]
        self.assertEqual(hub0.count("sel hub=255"), 4)
        self.assertIn("enable hub=0 flags=0 num=13", hub0)
        hub4 = out.split(" hub 4 ")[1].split(" hub 5 ")[0]
        self.assertIn("sel hub=0 flags=0 num=6", hub4)
        self.assertEqual(hub4.count("sel hub=255"), 1)
        self.assertIn("settle=5", hub4)
        # LED "Bi": pin2 on the MCP23017 (hub 7) pin GPB7 = 15
        self.assertIn("pin2 hub=7 flags=0 num=15", out)
        # PCF P0 button (hub 8 pin 0, inverted)
        self.assertIn("pin hub=8 flags=1 num=0\n  name=PCF 0", out)
        # mux digital button on channel 15 of the 4067 (hub 6)
        self.assertIn("pin hub=6 flags=1 num=15\n  name=Mux btn", out)
        self.assertIn("raw=4095..0 cc=27", out)
        self.assertIn("boot_combo=0x00000005", out)
        self.assertIn("matrix kind=0 keys=24 first=36 settle=1 rows=4 cols=6", out)
        self.assertIn("row hub=2 flags=0 num=3", out)
        self.assertIn("col hub=4 flags=0 num=5", out)
        self.assertIn("i2c sda=8 scl=9", out)
        self.assertIn("transport=1", out)
        self.assertIn("a hub=7 flags=2 num=0", out)   # MCP pull-up kept

    def test_presets_compile(self):
        for name, path, *_ in gb.list_presets():
            dev, diags = gd.load_device(path)
            self.assertIsNotNone(dev, (name, [str(d) for d in diags]))
            self.compile_with_core(gen_dir(dev, self.tmp, name))

    def test_empty_device_arrays(self):
        dev, diags = load(MINIMAL_ESP.format(plat="esp32", tr="serial") +
                          "analog:\n  - { id: k, pin: GPIO34, cc: 1 }\n")
        self.assertIsNotNone(dev, [str(d) for d in diags])
        cfg = gen_dir(dev, self.tmp)
        self.compile_with_core(cfg)
        out = self.dump(cfg, "empty")
        self.assertIn("buttons=0 (null)", out)
        self.assertIn("defaults0: 0 0 1 0", out)
        self.assertIn("matrix=null", out)
        self.assertIn("transport=2", out)
        self.assertIn("usb=0000:0000", out)


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestValidation(unittest.TestCase):
    def errs(self, text, *, level="error"):
        _dev, diags = load(text)
        return [d for d in diags if d.level == level]

    def assertError(self, text, pattern, line=None, path=None):
        es = self.errs(text)
        hits = [e for e in es if re.search(pattern, e.msg)]
        self.assertTrue(hits, f"no error matching {pattern!r}; got: {[str(e) for e in es]}")
        if line is not None:
            self.assertEqual(hits[0].line, line, str(hits[0]))
        if path is not None:
            self.assertEqual(hits[0].path, path, str(hits[0]))
        return hits[0]

    def assertClean(self, text):
        es = self.errs(text)
        self.assertEqual(es, [], [str(e) for e in es])

    def test_minimal_ok(self):
        self.assertClean(MINIMAL_NUC)

    def test_bad_pin_name(self):
        e = self.assertError(MINIMAL_NUC.replace("pin: PA1", "pin: PE9"), r"'PE9' is not a pin on nuvoton-nuc1xx",
                             line=8, path="buttons[0].pin")
        self.assertIn("valid: PA0-PA15, PB0-PB15", e.msg)
        self.assertError(MINIMAL_NUC.replace("pin: PA1", "pin: PA16"), r"not a pin")
        self.assertError(MINIMAL_ESP.format(plat="esp32", tr="serial") +
                         "buttons: [{ id: a, pin: GPIO24 }]\n", r"GPIO24.*valid: GPIO0-19, GPIO21-23")

    def test_str_format(self):
        _d, diags = gd.load_device("x.yaml", MINIMAL_NUC.replace("pin: PA1", "pin: PD99"))
        self.assertRegex(str(gd.errors(diags)[0]), r"^x\.yaml:8: error: buttons\[0\]\.pin: 'PD99'")

    def test_duplicate_pin(self):
        t = MINIMAL_NUC + "  - { id: b, pin: PA1 }\n"
        self.assertError(t, r"PA1 is already used by buttons\[0\]\.pin \(line 8\)", line=9)
        t = MINIMAL_NUC + "leds:\n  - { id: l, pin: PA1 }\n"
        self.assertError(t, r"PA1 is already used")

    def test_matrix_row_cant_be_button(self):
        t = MINIMAL_NUC + "key_matrix: { type: switch, rows: [PA1, PA2], columns: [PB0], keys: 2 }\n"
        self.assertError(t, r"PA1 is already used")

    def test_hub_pins_counted(self):
        t = MINIMAL_NUC + "sn74hc595:\n  - { id: s, data_pin: PA1, clock_pin: PA2, latch_pin: PA3 }\n"
        self.assertError(t, r"PA1 is already used")

    def test_allow_other_uses(self):
        def dev(o_clock, i_clock, o_latch="GPIO4", i_latch="GPIO4"):
            return (MINIMAL_ESP.format(plat="esp32s3", tr="usb") +
                    f"sn74hc595:\n  - {{ id: o, data_pin: GPIO1, clock_pin: {o_clock}, latch_pin: {o_latch} }}\n"
                    f"sn74hc165:\n  - {{ id: i, data_pin: GPIO5, clock_pin: {i_clock}, load_pin: {i_latch} }}\n"
                    "leds: [{ id: l, pin: { sn74hc595: o, number: 0 } }]\n"
                    "buttons: [{ id: b, pin: { sn74hc165: i, number: 0 } }]\n")
        share = "{ number: %s, allow_other_uses: true }"
        self.assertError(dev("GPIO2", "GPIO2"), r"GPIO2 is already used.*allow_other_uses: true on each use")
        self.assertError(dev(share % "GPIO2", "GPIO2"), r"GPIO2 is already used")
        self.assertClean(dev(share % "GPIO2", share % "GPIO2", share % "GPIO4", share % "GPIO4"))
        # shared, but the clock of one is the latch of the other: still an error
        self.assertError(dev(share % "GPIO2", share % "GPIO4", share % "GPIO4", share % "GPIO2"),
                         r"already used.*same control line")
        # only hub control lines can be shared
        t = MINIMAL_NUC.replace("pin: PA1", "pin: { number: PA1, allow_other_uses: true }")
        self.assertError(t, r"only shares hub control lines", path="buttons[0].pin.allow_other_uses")

    def test_unknown_hub(self):
        t = MINIMAL_NUC + "leds:\n  - { id: l, pin: { sn74hc595: nope, number: 0 } }\n"
        self.assertError(t, r"no hub with id 'nope'", line=10)
        t = MINIMAL_NUC + "  - { id: b, pin: { sn74hc595: l, number: 0 } }\nleds:\n  - { id: l, pin: PA2 }\n"
        self.assertError(t, r"'l' is the id of leds\[0\] \(an LED\), not of a hub")

    def test_hub_key_must_match_chip(self):
        hub = "pcf8574:\n  - { id: p }\ni2c: { sda: PA10, scl: PA11 }\n"
        self.assertError(MINIMAL_NUC + hub + "leds:\n  - { id: l, pin: { mcp23xxx: p, number: 0 } }\n",
                         r"hub 'p' is a pcf8574: write `pcf8574: p`")
        self.assertError(MINIMAL_NUC + hub + "leds:\n  - { id: l, pin: { pcf8574: p, mcp23xxx: p, number: 0 } }\n",
                         r"names more than one hub")
        self.assertError(MINIMAL_NUC + "mcp23017:\n  - { id: m }\ni2c: { sda: PA10, scl: PA11 }\n"
                         "leds:\n  - { id: l, pin: { mcp23017: m, number: 0 } }\n", r"`mcp23xxx:` \(as in ESPHome\)")

    def test_hub_range_and_type(self):
        hub = "sn74hc595:\n  - { id: s, data_pin: PA2, clock_pin: PA3, latch_pin: PA4, sr_count: 2 }\n"
        self.assertError(MINIMAL_NUC + hub + "leds:\n  - { id: l, pin: { sn74hc595: s, number: 16 } }\n",
                         r"pin 16 is out of range for hub 's' \(sn74hc595 \(2 chips x 8\): 0\.\.15\)")
        self.assertClean(MINIMAL_NUC + hub + "leds:\n  - { id: l, pin: { sn74hc595: s, number: 15 } }\n")
        self.assertError(MINIMAL_NUC.replace("pin: PA1", "pin: { sn74hc595: s, number: 0 }") + hub,
                         r"sn74hc595, which can't be an input")
        self.assertError(MINIMAL_NUC + hub.replace("sr_count: 2", "sr_count: 5"), r"5 is out of range \(1\.\.4\)")

    def test_mux(self):
        mux = "cd74hc4051:\n  - { id: m, pin_s0: PA2, pin_s1: PA3, pin_s2: PA4, common_pin: %s }\n"
        an = "analog:\n  - { id: k, pin: { cd74hc4051: m, number: %d }, cc: 1 }\n"
        self.assertClean(MINIMAL_NUC + mux % "PA5" + an % 7)
        self.assertError(MINIMAL_NUC + mux % "PA5" + an % 8, r"channel 8 is out of range")
        self.assertError(MINIMAL_NUC + mux % "PB5" + an % 0, r"common_pin PB5 has no ADC")
        # digital buttons on a non-ADC mux are fine
        self.assertClean(MINIMAL_NUC.replace("pin: PA1", "pin: { cd74hc4051: m, number: 3 }") + mux % "PB5")
        self.assertError(MINIMAL_NUC + mux.replace(", pin_s2: PA4", "") % "PA5" + an % 0,
                         r"hubs\.|cd74hc4051\[0\]\.pin_s2|is required")
        self.assertError(MINIMAL_NUC + mux.replace("common_pin: %s", "common_pin: { cd74hc4051: m, number: 1 }")
                         + an % 0, r"must be an MCU pin")
        self.assertError(MINIMAL_NUC + "cd74hc4067:\n  - { id: m, pin_s0: PA2, pin_s1: PA3, pin_s2: PA4, "
                         "common_pin: PA5 }\n", r"cd74hc4067\[0\]\.pin_s3|is required")

    def test_analog_on_non_adc(self):
        self.assertError(MINIMAL_NUC + "analog:\n  - { id: v, pin: PB0, cc: 7 }\n",
                         r"PB0 has no ADC on nuvoton-nuc1xx \(ADC pins: PA0-PA7", line=10)
        self.assertError(MINIMAL_ESP.format(plat="esp8266", tr="serial") +
                         "analog: [{ id: v, pin: GPIO4, cc: 7 }]\n", r"only ADC pin is A0")
        self.assertClean(MINIMAL_ESP.format(plat="esp8266", tr="serial") +
                         "analog: [{ id: v, pin: A0, cc: 7 }]\n")

    def test_adc2_and_flash_warnings(self):
        t = MINIMAL_ESP.format(plat="esp32", tr="serial") + "analog: [{ id: v, pin: GPIO25, cc: 7 }]\n" \
            "buttons: [{ id: b, pin: GPIO6 }]\n"
        ws = [w.msg for w in self.errs(t, level="warning")]
        self.assertTrue(any("ADC2" in w for w in ws), ws)
        self.assertTrue(any("SPI flash" in w for w in ws), ws)

    def test_input_only_and_usb_pins(self):
        self.assertError(MINIMAL_ESP.format(plat="esp32", tr="serial") + "leds: [{ id: l, pin: GPIO34 }]\n",
                         r"GPIO34 is input-only")
        self.assertError(MINIMAL_ESP.format(plat="esp32s3", tr="usb") + "buttons: [{ id: b, pin: GPIO19 }]\n",
                         r"native USB data line")
        self.assertError(MINIMAL_ESP.format(plat="esp8266", tr="usb"), r"'usb' is not available on esp8266")

    def test_pin_modes(self):
        btn = MINIMAL_NUC.replace("pin: PA1", "pin: { number: PA1, mode: %s }")
        self.assertClean(btn % "INPUT")
        self.assertClean(btn % "input_pullup")                     # NUC: quasi-bidirectional weak pull-up
        self.assertClean(btn % "{ input: true, pullup: true }")
        self.assertError(btn % "INPUT_PULLDOWN", r"NUC1xx GPIOs have no internal pull-down")
        self.assertError(btn % "OUTPUT", r"this pin is an input: mode can't include output")
        self.assertError(btn % "INPUT_PULUP", r"unknown mode 'INPUT_PULUP'.*did you mean INPUT_PULLUP")
        self.assertError(btn % "{ input: true, pullup: true, pulldown: true }", r"pullup and pulldown")
        self.assertError(btn % "INPUT_OUTPUT_OPEN_DRAIN", r"open_drain isn't supported")
        self.assertError(btn % "{ input: true, pulup: true }", r"unknown key \(did you mean 'pullup'")
        led = MINIMAL_NUC + "leds:\n  - { id: l, pin: { number: PA2, mode: %s } }\n"
        self.assertClean(led % "OUTPUT")
        self.assertError(led % "INPUT_PULLUP", r"this pin is an output: use mode: OUTPUT")
        an = MINIMAL_NUC + "analog:\n  - { id: v, pin: { number: PA0, mode: %s }, cc: 7 }\n"
        self.assertClean(an % "ANALOG")
        self.assertError(an % "INPUT_PULLUP", r"analog input takes mode ANALOG or INPUT")
        self.assertError(MINIMAL_ESP.format(plat="esp32", tr="serial") +
                         "buttons: [{ id: b, pin: { number: GPIO34, mode: INPUT_PULLUP } }]\n",
                         r"GPIO34 has no internal pull-up/down on esp32")
        hub = "sn74hc165:\n  - { id: s, data_pin: PA2, clock_pin: PA3, load_pin: PA4 }\n"
        self.assertError(MINIMAL_NUC.replace("pin: PA1", "pin: { sn74hc165: s, number: 0, mode: INPUT_PULLUP }")
                         + hub, r"a sn74hc165 pin has no pull-up \(only mcp23017")

    def test_time_periods(self):
        t = MINIMAL_NUC + "scan_interval: %s\n"
        for ok in ("125us", "0.5ms", "1 ms", "125µs"):
            self.assertClean(t % ok)
        dev, _ = load(t % "0.5ms")
        self.assertEqual(dev.scan_us, 500)
        self.assertError(t % "125", r"needs a unit: `scan_interval: 125` could be any unit; write e\.g\. "
                                    r"`scan_interval: 125us`", path="scan_interval")
        self.assertError(t % "125 parsecs", r"must be a time period like 125us")
        self.assertError(t % "70ms", r"70ms is out of range \(1us\.\.65535us\)")
        ws = self.errs(t % "1.5us", level="warning")
        self.assertTrue(any("rounded to 2us" in w.msg for w in ws), [str(w) for w in ws])

    def test_bad_action(self):
        e = self.assertError(MINIMAL_NUC.replace("cc_momentary", "cc_momentry"), r"unknown action 'cc_momentry'")
        self.assertIn("did you mean 'cc_momentary'", e.msg)

    def test_numbers(self):
        self.assertError(MINIMAL_NUC.replace("cc: 1", "cc: 128"), r"128 is out of range \(0\.\.127\)")
        self.assertError(MINIMAL_NUC.replace("0x3F01", "0x4000"), r"0x4000 is out of range \(0x0\.\.0x3FFF\)")
        self.assertError(MINIMAL_NUC.replace("cc: 1", "cc: yes"), r"must be an integer 0\.\.127 \(got boolean 'yes'\)")

    def test_button_cc_and_note(self):
        btn = MINIMAL_NUC.replace("action: cc_momentary, cc: 1", "%s")
        self.assertClean(btn % "action: note, note: 60")
        self.assertError(btn % "action: note, cc: 60", r"action note takes note:, not cc:")
        self.assertError(btn % "action: cc_toggle, note: 60", r"action cc_toggle takes cc:, not note:")
        self.assertError(btn % "action: cc_toggle", r"action cc_toggle needs cc: \(0\.\.127\)")
        self.assertError(btn % "action: note, note: 1, cc: 2", r"give cc: or note:, not both")
        ws = self.errs(btn % "action: octave_up, cc: 5", level="warning")
        self.assertTrue(any("sends no CC or note" in w.msg for w in ws), ws)
        dev, _ = load(btn % "action: note, note: 61")
        self.assertEqual(dev.buttons[0].number, 61)

    def test_matrix(self):
        m = MINIMAL_NUC + "key_matrix:\n  type: %s\n  rows: [PA8, PA9]\n  columns: [PB0, PB1, PB2%s]\n  keys: %d\n"
        self.assertError(m % ("velocity", "", 3), r"even number of columns")
        self.assertError(m % ("velocity", ", PB3", 5), r"5 keys don't fit a 2 x 4 velocity matrix \(at most 4\)")
        self.assertClean(m % ("velocity", ", PB3", 4))
        self.assertError(m % ("switch", "", 7), r"at most 6")
        self.assertError((m % ("switch", "", 6)) + "  first_note: 123\n", r"goes past note 127")
        self.assertError(m % ("diagonal", "", 6), r"'diagonal' is unknown")
        dev, _ = load((m % ("switch", "", 6)) + "  settle: 3us\n")
        self.assertEqual(dev.matrix.settle_us, 3)
        self.assertError((m % ("switch", "", 6)) + "  settle: 1ms\n", r"1ms is out of range \(0us\.\.255us\)")

    def test_version(self):
        dev, diags = gd.load_device("x.yaml", MINIMAL_NUC + "project: { version: 1.2.300 }\n")
        self.assertFalse(gd.errors(diags), diags)
        h = gd.generate(dev, "x.yaml")["gm_device_config.h"]
        self.assertIn("#define GM_FW_VERSION 0x0102012Cu", h)
        self.assertIn('#define GM_FW_VERSION_STR "1.2.300"', h)
        dev, _ = gd.load_device("x.yaml", MINIMAL_NUC)
        self.assertNotIn("GM_FW_VERSION", gd.generate(dev, "x.yaml")["gm_device_config.h"])
        self.assertError(MINIMAL_NUC + "project: { version: 1.0 }\n", r'quote it', path="project.version")
        self.assertError(MINIMAL_NUC + "project: { version: v1.0.0 }\n", r"must be major\.minor\.patch")
        self.assertError(MINIMAL_NUC + "project: { version: 256.0.0 }\n", r"out of range")
        self.assertError(MINIMAL_NUC + "project: { version: 1.0.65536 }\n", r"out of range")
        self.assertError(MINIMAL_NUC + "project: { name: x }\n", r"unknown key", path="project.name")
        self.assertError(MINIMAL_NUC + "version: 1.0.0\n", r"moved: use `project: \{ version", path="version")

    def test_matrix_velocity_defaults(self):
        m = MINIMAL_NUC + "key_matrix:\n  type: %s\n  rows: [PA8, PA9]\n  columns: [PB0, PB1, PB2, PB3]\n  keys: 4\n"
        vel = m % "velocity"
        dev, diags = gd.load_device("x.yaml", vel + "  velocity: { fast: 2.0ms, slow: 80ms }\n")
        self.assertFalse(gd.errors(diags), diags)
        h = gd.generate(dev, "x.yaml")["gm_device_config.h"]
        self.assertIn("#define GM_VEL_FAST_DEFAULT 20 /* 2 ms */", h)
        self.assertIn("#define GM_VEL_SLOW_DEFAULT 800 /* 80 ms */", h)
        dev, _ = gd.load_device("x.yaml", vel + "  velocity: { slow: 60ms }\n")
        h = gd.generate(dev, "x.yaml")["gm_device_config.h"]
        self.assertNotIn("GM_VEL_FAST_DEFAULT", h)
        self.assertIn("#define GM_VEL_SLOW_DEFAULT 600", h)
        dev, _ = gd.load_device("x.yaml", vel + "  velocity: { fast: 1500us, slow: 0.1s }\n")
        h = gd.generate(dev, "x.yaml")["gm_device_config.h"]
        self.assertIn("#define GM_VEL_FAST_DEFAULT 15", h)
        self.assertIn("#define GM_VEL_SLOW_DEFAULT 1000", h)
        dev, _ = gd.load_device("x.yaml", vel)
        self.assertNotIn("GM_VEL_", gd.generate(dev, "x.yaml")["gm_device_config.h"])
        self.assertError(vel + "  velocity: { fast: 0ms }\n", r"0ms is out of range \(100us\.\.50ms\)")
        self.assertError(vel + "  velocity: { slow: 300ms }\n", r"out of range \(2ms\.\.200ms\)")
        self.assertError(vel + "  velocity: { fast: 10ms, slow: 5ms }\n", r"must be shorter than slow")
        self.assertError(vel + "  velocity: { fast: 2 }\n", r"needs a unit")
        self.assertError(vel + "  velocity: { fst: 2ms }\n", r"unknown key")
        self.assertError((m % "switch") + "  velocity: { fast: 2ms }\n", r"only for type: velocity")

    def test_matrix_hub_restrictions(self):
        hubs = ("cd74hc4051:\n  - { id: m, pin_s0: PA2, pin_s1: PA3, pin_s2: PA4, common_pin: PA5 }\n"
                "mcp23017:\n  - { id: x, address: 0x20 }\ni2c: { sda: PA10, scl: PA11 }\n")
        t = MINIMAL_NUC + hubs + "key_matrix: { type: switch, rows: [PA8], columns: [{ cd74hc4051: m, number: 0 }] }\n"
        self.assertError(t, r"timer interrupt")
        t = MINIMAL_NUC + hubs + "key_matrix: { type: switch, rows: [{ mcp23xxx: x, number: 0 }], columns: [PB0] }\n"
        self.assertError(t, r"mcp23017, which can't be an output.*timer interrupt")

    def test_encoders(self):
        hubs = ("cd74hc4051:\n  - { id: m, pin_s0: PA2, pin_s1: PA3, pin_s2: PA4, common_pin: PA5 }\n"
                "mcp23017:\n  - { id: x }\ni2c: { sda: PA10, scl: PA11 }\n")
        self.assertError(MINIMAL_NUC + hubs + "encoders:\n  - { id: e, pin_a: { cd74hc4051: m, number: 0 }, "
                         "pin_b: { cd74hc4051: m, number: 1 }, cc: 1 }\n", r"encoders can't be on a mux")
        t = MINIMAL_NUC + hubs + "encoders:\n  - { id: e, pin_a: { mcp23xxx: x, number: 0 }, " \
            "pin_b: { mcp23xxx: x, number: 1 }, cc: 1 }\n" \
            "analog: [{ id: k, pin: { cd74hc4051: m, number: 0 }, cc: 2 }]\n"
        self.assertClean(t)
        self.assertTrue(any("miss steps" in w.msg for w in self.errs(t, level="warning")))

    def test_too_many_buttons(self):
        t = MINIMAL_NUC.split("buttons:")[0] + \
            "sn74hc165:\n  - { id: i, data_pin: PA2, clock_pin: PA3, load_pin: PA4, sr_count: 4 }\nbuttons:\n" + \
            "".join(f"  - {{ id: b{i}, pin: {{ sn74hc165: i, number: {i} }} }}\n" for i in range(32))
        self.assertClean(t)
        t += "  - { id: x, pin: PB0 }\n"
        self.assertError(t, r"33 buttons: at most 32")

    def test_i2c_required(self):
        t = MINIMAL_NUC + "pcf8574:\n  - { id: p }\nleds: [{ id: l, pin: { pcf8574: p, number: 0 } }]\n"
        self.assertError(t, r"needs the I2C pins")
        self.assertClean(t + "i2c: { sda: PA10, scl: PA11 }\n")
        self.assertError(t + "i2c: { sda: PA10, scl: PA1 }\n", r"PA1 is already used")
        t2 = t.replace("  - { id: p }\n", "  - { id: p }\n  - { id: q }\n")
        self.assertError(t2 + "i2c: { sda: PA10, scl: PA11 }\n", r"address 0x20 is already used")

    # -- ids, names, references --

    def test_refs(self):
        self.assertError(MINIMAL_NUC.replace("cc: 1", "cc: 1, led: nope"), r"no LED with id 'nope' \(LEDs: none\)")
        t = MINIMAL_NUC.replace("cc: 1", "cc: 1, led: edit_lde") + "leds: [{ id: edit_led, pin: PA2 }]\n"
        self.assertError(t, r"no LED with id 'edit_lde'.*did you mean 'edit_led'")
        self.assertError(MINIMAL_NUC.replace("cc: 1", "cc: 1, led: a"), r"'a' is the id of buttons\[0\] "
                                                                         r"\(a button\), not of an LED")
        self.assertError(MINIMAL_NUC + "boot_combo: [a, z]\n", r"no button with id 'z'", path="boot_combo[1]")
        self.assertError(MINIMAL_NUC + "boot_combo: [a, no]\n", r"YAML reads `no` as the boolean False")
        self.assertClean(MINIMAL_NUC + "boot_combo: [a]\n")

    def test_id_required_and_unique(self):
        self.assertError(MINIMAL_NUC.replace("id: a, ", ""), r"needs id: or name:", path="buttons[0]")
        e = self.assertError(MINIMAL_NUC + "  - { id: a, pin: PA2 }\n",
                             r"id 'a' of buttons\[1\] is already the id of buttons\[0\] \(line 8, a button\)")
        self.assertEqual(e.line, 9)
        # one namespace: an LED and a button can't share an id, nor a hub and an analog input
        self.assertError(MINIMAL_NUC + "leds: [{ id: a, pin: PA2 }]\n", r"already the id of")
        self.assertError(MINIMAL_NUC + "cd74hc4051:\n  - { id: v, pin_s0: PA2, pin_s1: PA3, pin_s2: PA4, "
                         "common_pin: PA5 }\nanalog: [{ id: v, pin: { cd74hc4051: v, number: 0 }, cc: 1 }]\n",
                         r"id 'v' of analog\[0\] is already the id of cd74hc4051\[0\]")
        self.assertError(MINIMAL_NUC + "pcf8574:\n  - { address: 0x20 }\n", r"needs id:")

    def test_id_rules(self):
        bad = {"Edit": r"must be lower_snake_case", "EditButton": r"must be lower_snake_case",
               "oct-down": r"isn't a valid id: a-z, 0-9 and _ only", "1st": r"read by YAML as|isn't a valid id",
               "_x": r"isn't a valid id", "edit led": r"isn't a valid id", "int": r"C/C\+\+ keyword",
               "class": r"C/C\+\+ keyword"}
        for ident, pat in bad.items():
            self.assertError(MINIMAL_NUC.replace("id: a", f"id: {ident}"), pat, path="buttons[0].id")
        e = self.assertError(MINIMAL_NUC.replace("id: a", "id: Oct - LED"), r"isn't a valid id")
        self.assertIn("e.g. `id: oct_minus_led`", e.msg)
        self.assertError(MINIMAL_NUC + "id_style: c_identifier\n", r"unknown key", path="id_style")

    def test_yaml_ambiguous_ids(self):
        """PyYAML is YAML 1.1: bare no/on/off/yes are booleans, null is None, 0x10 is 16."""
        for word, pat in (("no", r"`no` is read by YAML as the boolean false \(YAML 1\.1"),
                          ("On", r"`On` is read by YAML as the boolean true"),
                          ("off", r"boolean false"), ("null", r"`null` is read by YAML as null"),
                          ("12", r"`12` is read by YAML as the number 12; an id must start with a letter"),
                          ("0x1F", r"the number 31")):
            self.assertError(MINIMAL_NUC.replace("id: a", f"id: {word}"), pat, path="buttons[0].id")
        # quoted, or y/n (PyYAML reads those as strings): still refused, other YAML tools differ
        self.assertError(MINIMAL_NUC.replace("id: a", "id: 'no'"), r"'no' can't be an id: written without quotes")
        self.assertError(MINIMAL_NUC.replace("id: a", "id: y"), r"'y' can't be an id.*boolean")
        self.assertError(MINIMAL_NUC.replace("id: a", "id: 'null'"), r"as a null")

    def test_names(self):
        dev, diags = load(MINIMAL_NUC.replace("id: a", "id: oct_down"))
        self.assertEqual(dev.buttons[0].name, "Oct down")
        dev, _ = load(MINIMAL_NUC.replace("id: a", "id: oct_down, name: Oct -"))
        self.assertEqual((dev.buttons[0].id, dev.buttons[0].name), ("oct_down", "Oct -"))
        # only a name: the id is derived from it (ESPHome), and others can refer to it
        dev, diags = load(MINIMAL_NUC.replace("id: a", "name: Pitch Down!") + "boot_combo: [pitch_down]\n")
        self.assertFalse(gd.errors(diags), [str(d) for d in diags])
        self.assertEqual((dev.buttons[0].id, dev.buttons[0].name), ("pitch_down", "Pitch Down!"))
        e = self.assertError(MINIMAL_NUC.replace("id: a", "name: Oct -") + "  - { name: Oct +, pin: PA2 }\n",
                             r"id 'oct' of buttons\[1\] \(derived from its name 'Oct \+'\) is already the id of "
                             r"buttons\[0\] \(derived from its name 'Oct -'\)")
        self.assertEqual(e.path, "buttons[1].name")
        self.assertError(MINIMAL_NUC.replace("id: a", "name: '42'"), r"derived from name '42' would be '42'.*set id:")
        ws = self.errs(MINIMAL_NUC + "  - { id: b, name: A, pin: PA2 }\n", level="warning")
        self.assertTrue(any("'A' is also the name of buttons[0]" in w.msg for w in ws), [str(w) for w in ws])

    def test_device_names(self):
        dev, _ = load(MINIMAL_NUC + "friendly_name: My Keys\n")
        files = gd.generate(dev, "x.yaml")["gm_device_config.h"]
        self.assertIn('#define GM_DEVICE_NAME "My Keys"', files)
        self.assertIn('#define GM_NODE_NAME "test"', files)
        dev, _ = load(MINIMAL_NUC)
        self.assertEqual(dev.name, "test")          # friendly_name defaults to the name
        self.assertError(MINIMAL_NUC.replace("name: test", "name: Keystation Mini 32"),
                         r"isn't a node name.*`name: keystation-mini-32`; put 'Keystation Mini 32' in friendly_name")
        self.assertError(MINIMAL_NUC.replace("name: test", "name: -x"), r"isn't a node name")
        self.assertError(MINIMAL_NUC.replace("name: test", "name: " + "a" * 32), r"at most 31")
        self.assertClean(MINIMAL_NUC.replace("name: test", "name: " + "a" * 31))

    def test_format_1_errors(self):
        """An old file: the version error, then one error per old key, at its line, saying what replaced it."""
        old = ("gemifi: 1\nname: test\nboard_id: 0x3F01\nplatform: nuvoton-nuc1xx\nchip: NUC120LC1BN\n"
               "scan_us: 125\n"
               "hubs:\n  - { id: m, type: 74hc4051, select: [PA2, PA3, PA4], common: PA5 }\n"
               "buttons:\n  - { label: Edit, pin: { pin: PA1, inverted: true, pullup: true }, "
               "action: cc_toggle, number: 80 }\n"
               "analog:\n  - { label: Vol, pin: { hub: m, channel: 0 }, cc: 7, raw_min: 32 }\n"
               "encoders:\n  - { label: E, a: PB0, b: PB1, cc: 1 }\n"
               "key_matrix: { kind: switch, rows: [PA8], cols: [PB2], settle_us: 1 }\n")
        es = {(e.path, e.line): e.msg for e in self.errs(old)}
        expect = {
            ("gemifi", 1): r"format 1 file; this builder reads format 2",
            ("scan_us", 6): r"renamed: use `scan_interval:` with a unit",
            ("hubs", 7): r"declare each hub under its chip's key, ESPHome style",
            ("buttons[0].label", 10): r"renamed: use `id:`",
            ("buttons[0].pin.pin", 10): r"renamed: use `number:`",
            ("buttons[0].pin.pullup", 10): r"use `mode: INPUT_PULLUP`",
            ("buttons[0].number", 10): r"renamed: use `cc:`",
            ("analog[0].label", 12): r"renamed: use `id:`",
            ("encoders[0].label", 14): r"renamed: use `id:`",
            ("encoders[0].b", 14): r"renamed: use `pin_b:`",
            ("analog[0].pin.hub", 12): r"name the hub by its chip",
            ("analog[0].pin.channel", 12): r"renamed: use `number:`",
            ("analog[0].raw_min", 12): r"renamed: use `adc_min:`",
            ("encoders[0].a", 14): r"renamed: use `pin_a:`",
            ("key_matrix.kind", 15): r"renamed: use `type:`",
            ("key_matrix.cols", 15): r"renamed: use `columns:`",
            ("key_matrix.settle_us", 15): r"renamed: use `settle:`",
        }
        for key, pat in expect.items():
            self.assertIn(key, es, f"{key} not reported; got {es}")
            self.assertRegex(es[key], pat)
        # nothing else: the renames don't cascade into "is required" noise
        self.assertEqual(set(es) - set(expect), set(), es)
        hub = ("gemifi: 2\nname: t\nboard_id: 1\nplatform: nuvoton-nuc1xx\nchip: NUC120LC1BN\n"
               "sn74hc595:\n  - { id: s, type: 74hc595, data: PA1, clock_pin: PA2, latch_pin: PA3, shared: true }\n")
        es = {e.path: e.msg for e in self.errs(hub)}
        self.assertRegex(es["sn74hc595[0].type"], r"the chip is the list")
        self.assertRegex(es["sn74hc595[0].data"], r"use `data_pin:`")
        self.assertRegex(es["sn74hc595[0].shared"], r"allow_other_uses: true")

    def test_hub_ids(self):
        t = MINIMAL_NUC + ("pcf8574:\n  - { id: p }\n  - { id: p, address: 0x21 }\n"
                           "i2c: { sda: PA10, scl: PA11 }\n")
        self.assertError(t, r"id 'p' of pcf8574\[1\] is already the id of pcf8574\[0\]")
        self.assertError(MINIMAL_NUC + "sn74hc999:\n  - { id: p }\n", r"unknown key")
        self.assertError(MINIMAL_NUC + "pcf8574:\n  - { id: p, pin_s0: PA2 }\ni2c: { sda: PA10, scl: PA11 }\n",
                         r"unknown key")

    def test_platform_and_keys(self):
        self.assertError(MINIMAL_NUC.replace("nuvoton-nuc1xx", "esp33"), r"unknown platform 'esp33'")
        e = self.assertError(MINIMAL_NUC + "scan_intervall: 100us\n", r"unknown key \(did you mean 'scan_interval'")
        self.assertEqual(e.line, 9)
        self.assertError(MINIMAL_NUC.replace("gemifi: 2", "gemifi: 3"), r"format version 3")
        self.assertError(MINIMAL_NUC.replace("pin: PA1,", "pin: { number: PA1, invert: true },"),
                         r"did you mean 'inverted'")

    def test_yaml_errors(self):
        _d, diags = load(MINIMAL_NUC + "leds: [\n")
        self.assertEqual(len(diags), 1)
        self.assertIn("YAML syntax", diags[0].msg)
        _d, diags = load(MINIMAL_NUC + "name: again\n")
        self.assertIn("duplicate key 'name' (first on line 2)", diags[0].msg)
        self.assertEqual(diags[0].line, 9)
@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestCli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gb.main(list(args))
        return rc, out.getvalue(), err.getvalue()

    def test_validate(self):
        rc, out, _ = self.run_cli("validate", KEYSTATION)
        self.assertEqual(rc, 0)
        self.assertIn("OK: Keystation Mini 32", out)
        bad = os.path.join(self.tmp, "bad.yaml")
        with open(bad, "w") as f:
            f.write(MINIMAL_NUC.replace("PA1", "PZ1"))
        rc, _, err = self.run_cli("validate", bad)
        self.assertEqual(rc, 1)
        self.assertRegex(err, r"bad\.yaml:8: error: buttons\[0\]\.pin: 'PZ1'")

    def test_generate(self):
        rc, _, _ = self.run_cli("generate", KEYSTATION, "-o", self.tmp)
        self.assertEqual(rc, 0)
        with open(os.path.join(self.tmp, "gm_device_config.c")) as f:
            self.assertIn("generated from devices/keystation-mini32-mk3.yaml", f.read())

    def test_presets_and_new(self):
        rc, out, _ = self.run_cli("presets")
        self.assertEqual(rc, 0)
        self.assertRegex(out, r"keystation-mini32-mk3\s+nuvoton-nuc1xx\s+M-Audio")
        rc, out, _ = self.run_cli("new", "mine", "--preset", "keystation-mini32-mk3", "--dir", self.tmp)
        self.assertEqual(rc, 0)
        p = os.path.join(self.tmp, "mine.yaml")
        self.assertTrue(os.path.exists(p))
        dev, _ = gd.load_device(p)
        self.assertIsNotNone(dev)
        self.assertEqual((dev.node_name, dev.name), ("mine", "Keystation Mini 32"))   # name follows the file
        rc, _, _ = self.run_cli("new", "My.Keys", "--preset", "keystation-mini32-mk3", "--dir", self.tmp)
        dev, _ = gd.load_device(os.path.join(self.tmp, "My.Keys.yaml"))
        self.assertEqual(dev.node_name, "keystation-mini32")      # not a node name: the preset's is kept
        self.assertEqual(gb.peek(p)[2], "Keystation Mini 32")     # the list shows friendly_name
        rc, _, err = self.run_cli("new", "mine", "--preset", "keystation-mini32-mk3", "--dir", self.tmp)
        self.assertEqual(rc, 1)
        self.assertIn("already exists", err)
        rc, _, err = self.run_cli("new", "../evil", "--preset", "keystation-mini32-mk3", "--dir", self.tmp)
        self.assertEqual(rc, 1)
        rc, _, err = self.run_cli("new", "x", "--preset", "nope", "--dir", self.tmp)
        self.assertIn("unknown preset", err)


def start_server(roots, **opts):
    """Dashboard on an ephemeral loopback port in a thread. Returns (server, port)."""
    import dashboard
    from http.server import ThreadingHTTPServer
    srv = ThreadingHTTPServer(("127.0.0.1", 0), None)
    port = srv.server_address[1]
    handler = dashboard.make_handler(roots, port, **opts)
    handler.log_message = lambda *a: None
    srv.RequestHandlerClass = handler
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, port


def raw_req(port, method, path, body=None, headers=None):
    """http.client request with the path sent verbatim. Returns (status, headers, body)."""
    import http.client
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
    h = {"Host": f"127.0.0.1:{port}"}
    if body is not None:
        h["Content-Type"] = "application/json"
        body = json.dumps(body).encode()
        h["Content-Length"] = str(len(body))
    h.update(headers or {})
    c.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
    for k, v in h.items():
        c.putheader(k, v)
    c.endheaders(body)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, {k.lower(): v for k, v in r.getheaders()}, data


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestDashboard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import dashboard
        cls.dashboard = dashboard
        cls.tmp = tempfile.mkdtemp()
        cls.mine = os.path.join(cls.tmp, "mine")
        cls.roots = dashboard.Roots(cls.mine, os.path.join(cls.tmp, "build"), readonly_presets=True)
        cls.srv, cls.port = start_server(cls.roots)

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()
        shutil.rmtree(cls.tmp)

    def req(self, path, body=None, host=None, headers=None):
        h = dict(headers or {})
        if host:
            h["Host"] = host
        st, _h, data = raw_req(self.port, "GET" if body is None else "POST", path, body, h)
        return st, data

    def fake_image(self, ident, name, data=b"\x01\x02"):
        d = self.roots.out_dir(ident)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)

    def test_resolve_refuses_traversal(self):
        for bad in ["../x.yaml", "mine/../x.yaml", "mine/..", "etc/passwd", "mine/a/b.yaml", "mine/.x.yaml",
                    "presets/../../etc/passwd", "mine/x.txt", None, "mine/x.yaml/"]:
            self.assertIsNone(self.roots.resolve(bad), bad)
        self.assertTrue(self.roots.resolve("mine/ok.yaml").startswith(os.path.realpath(self.mine)))

    def test_api(self):
        st, body = self.req("/")
        self.assertEqual(st, 200)
        self.assertIn(b"gemifi dashboard", body)
        st, body = self.req("/api/devices")
        ids = [d["id"] for d in json.loads(body)["devices"]]
        self.assertIn("presets/keystation-mini32-mk3.yaml", ids)
        st, body = self.req("/api/file?id=presets%2F..%2F..%2FREADME.md")
        self.assertEqual(st, 400)
        st, _ = self.req("/api/devices", host="evil.example:80")
        self.assertEqual(st, 403)
        st, body = self.req("/api/new", {"name": "dash", "preset": "keystation-mini32-mk3"})
        self.assertEqual(st, 200, body)
        self.assertEqual(json.loads(body)["id"], "mine/dash.yaml")
        st, body = self.req("/api/validate", {"text": MINIMAL_NUC.replace("PA1", "PQ1")})
        j = json.loads(body)
        self.assertFalse(j["ok"])
        self.assertEqual(j["diags"][0]["line"], 8)
        st, body = self.req("/api/save", {"id": "mine/dash.yaml", "text": MINIMAL_NUC})
        self.assertEqual(st, 200)
        st, body = self.req("/api/file?id=mine%2Fdash.yaml")
        self.assertEqual(json.loads(body)["text"], MINIMAL_NUC)
        st, _ = self.req("/download?id=mine%2Fdash.yaml&file=..%2F..%2Fx.gfw")
        self.assertEqual(st, 404)

    def test_info_and_presets(self):
        st, body = self.req("/api/info")
        self.assertEqual(st, 200)
        j = json.loads(body)
        self.assertEqual((j["name"], j["version"], j["user_dir"]), ("gemifi builder", "0.1.0", self.mine))
        self.assertEqual(set(j["toolchains"]), {"nuvoton-nuc1xx", "esp"})
        self.assertTrue(all(isinstance(v, bool) for v in j["toolchains"].values()))
        st, body = self.req("/api/presets")
        names = [p["name"] for p in json.loads(body)["presets"]]
        self.assertIn("keystation-mini32-mk3", names)
        st, body = self.req("/api/nope")
        self.assertEqual((st, json.loads(body)), (404, {"error": "not found"}))

    def test_post_needs_json(self):
        st, body = self.req("/api/save", {"id": "mine/x.yaml", "text": ""}, headers={"Content-Type": "text/plain"})
        self.assertEqual(st, 415)
        self.assertIn("error", json.loads(body))
        st, _ = self.req("/api/save", [1, 2])
        self.assertEqual(st, 400)

    def test_readonly_presets(self):
        st, body = self.req("/api/devices")
        devs = {d["id"]: d for d in json.loads(body)["devices"]}
        self.assertFalse(devs["presets/keystation-mini32-mk3.yaml"]["writable"])
        with open(KEYSTATION) as f:
            text = f.read()
        st, body = self.req("/api/save", {"id": "presets/keystation-mini32-mk3.yaml", "text": text})
        self.assertEqual(st, 403)
        self.assertIn("error", json.loads(body))
        st, _ = self.req("/api/delete", {"id": "presets/keystation-mini32-mk3.yaml"})
        self.assertEqual(st, 403)
        self.assertTrue(os.path.exists(KEYSTATION))
        st, body = self.req("/api/new", {"name": "copy", "preset": "keystation-mini32-mk3"})
        self.assertEqual((st, json.loads(body)["id"]), (200, "mine/copy.yaml"))
        st, body = self.req("/api/devices")
        devs = {d["id"]: d for d in json.loads(body)["devices"]}
        self.assertTrue(devs["mine/copy.yaml"]["writable"])
        self.assertEqual(devs["mine/copy.yaml"]["root"], "mine")

    def test_writable_presets_when_allowed(self):
        tmp = tempfile.mkdtemp()
        try:
            roots = self.dashboard.Roots(tmp, os.path.join(tmp, "b"), preset_dir=tmp, readonly_presets=False)
            with open(os.path.join(tmp, "p.yaml"), "w") as f:
                f.write(MINIMAL_NUC)
            self.assertTrue(roots.writable("presets/p.yaml"))
            os.chmod(tmp, 0o555)
            try:
                if os.access(tmp, os.W_OK):   # root ignores permissions
                    self.skipTest("running as root")
                self.assertTrue(self.dashboard.Roots(tmp, preset_dir=tmp, readonly_presets=False).readonly_presets)
            finally:
                os.chmod(tmp, 0o755)
        finally:
            shutil.rmtree(tmp)

    def test_out_dirs_per_id(self):
        a, b = self.roots.out_dir("presets/x.yaml"), self.roots.out_dir("mine/x.yaml")
        self.assertNotEqual(a, b)
        self.assertEqual(os.path.basename(a), "x")
        self.assertEqual(b, os.path.join(self.tmp, "build", "mine", "x"))

    def test_image_and_delete(self):
        st, _ = self.req("/api/new", {"name": "gone", "preset": "keystation-mini32-mk3"})
        self.assertEqual(st, 200)
        self.fake_image("mine/gone.yaml", "gone.gfw", b"GFW!")
        self.fake_image("mine/gone.yaml", "notes.txt")
        st, body = self.req("/api/devices")
        devs = {d["id"]: d for d in json.loads(body)["devices"]}
        self.assertEqual(devs["mine/gone.yaml"]["images"], ["gone.gfw"])
        self.assertEqual(self.roots.images("presets/gone.yaml"), [])   # not shared with mine/
        for path in ("/api/image", "/download"):
            st, h, data = raw_req(self.port, "GET", path + "?id=mine%2Fgone.yaml&file=gone.gfw")
            self.assertEqual((st, data), (200, b"GFW!"))
            self.assertEqual(h["content-type"], "application/octet-stream")
            self.assertEqual(h["content-disposition"], 'attachment; filename="gone.gfw"')
        st, _ = self.req("/api/image?id=mine%2Fgone.yaml&file=notes.txt")
        self.assertEqual(st, 404)
        st, _ = self.req("/api/image?id=presets%2Fgone.yaml&file=gone.gfw")
        self.assertEqual(st, 404)
        st, body = self.req("/api/delete", {"id": "mine/gone.yaml"})
        self.assertEqual((st, json.loads(body)), (200, {"ok": True}))
        self.assertFalse(os.path.exists(os.path.join(self.mine, "gone.yaml")))
        self.assertFalse(os.path.exists(self.roots.out_dir("mine/gone.yaml")))
        st, _ = self.req("/api/delete", {"id": "mine/gone.yaml"})
        self.assertEqual(st, 404)
        st, _ = self.req("/api/delete", {"id": "mine/../x.yaml"})
        self.assertEqual(st, 400)

    def test_stale_after_edit(self):
        gb = self.dashboard.gb
        st, _ = self.req("/api/new", {"name": "fresh", "preset": "keystation-mini32-mk3"})
        self.assertEqual(st, 200)
        ident = "mine/fresh.yaml"
        path = self.roots.resolve(ident)

        def stale():
            st, body = self.req("/api/devices")
            return {d["id"]: d for d in json.loads(body)["devices"]}[ident]["stale"]

        self.assertFalse(stale())                       # not built: not stale either
        self.fake_image(ident, "fresh.gfw")
        self.assertTrue(stale())                        # images with no recorded source
        with open(os.path.join(self.roots.out_dir(ident), gb.SOURCE_HASH_FILE), "w") as f:
            f.write(gb.source_hash(path) + "\n")
        self.assertFalse(stale())
        with open(path, encoding="utf-8") as f:
            text = f.read()
        st, _ = self.req("/api/save", {"id": ident, "text": text + "# edited\n"})
        self.assertEqual(st, 200)
        self.assertTrue(stale())
        st, _ = self.req("/api/save", {"id": ident, "text": text})   # edit undone
        self.assertFalse(stale())

    def test_build_busy(self):
        self.assertTrue(self.dashboard._build_lock.acquire(blocking=False))
        try:
            st, body = self.req("/api/build", {"id": "presets/keystation-mini32-mk3.yaml"})
        finally:
            self.dashboard._build_lock.release()
        self.assertEqual(st, 409)
        self.assertIn("error", json.loads(body))
        st, _ = self.req("/api/build", {"id": "mine/missing.yaml"})
        self.assertEqual(st, 404)

    @unittest.skipUnless(shutil.which("arm-none-eabi-gcc") and shutil.which("make"), "needs arm-none-eabi-gcc")
    def test_build_nuc(self):
        st, body = self.req("/api/build", {"id": "presets/keystation-mini32-mk3.yaml"})
        log = body.decode()
        self.assertEqual(st, 200, log)
        self.assertEqual(log.strip().splitlines()[-1], "[exit 0]", log)
        imgs = self.roots.images("presets/keystation-mini32-mk3.yaml")
        self.assertEqual(imgs, ["keystation-mini32-mk3.bin", "keystation-mini32-mk3.gfw"])
        st, _h, data = raw_req(self.port, "GET", "/api/image?id=presets%2Fkeystation-mini32-mk3.yaml"
                               "&file=keystation-mini32-mk3.gfw")
        self.assertEqual(st, 200)
        self.assertGreater(len(data), 1024)


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestDashboardServing(unittest.TestCase):
    def setUp(self):
        import dashboard
        self.dashboard = dashboard
        self.tmp = tempfile.mkdtemp()
        self.web = os.path.join(self.tmp, "web")
        os.makedirs(os.path.join(self.web, "assets"))
        with open(os.path.join(self.web, "index.html"), "w") as f:
            f.write("<!doctype html><title>updater</title>")
        with open(os.path.join(self.web, "assets", "app-1234.js"), "w") as f:
            f.write("console.log(1)")
        with open(os.path.join(self.web, "assets", "app-1234.css"), "w") as f:
            f.write("body{}")
        with open(os.path.join(self.tmp, "secret.txt"), "w") as f:
            f.write("secret")
        self.roots = dashboard.Roots(os.path.join(self.tmp, "mine"), os.path.join(self.tmp, "build"))
        self.servers = []

    def tearDown(self):
        for s in self.servers:
            s.shutdown()
            s.server_close()
        shutil.rmtree(self.tmp)

    def start(self, **opts):
        srv, port = start_server(self.roots, **opts)
        self.servers.append(srv)
        return port

    def test_static(self):
        port = self.start(web_dir=self.web)
        st, h, body = raw_req(port, "GET", "/")
        self.assertEqual((st, body), (200, b"<!doctype html><title>updater</title>"))
        self.assertEqual(h["content-type"], "text/html; charset=utf-8")
        self.assertEqual(h["cache-control"], "no-cache")
        st, h, body = raw_req(port, "GET", "/assets/app-1234.js")
        self.assertEqual((st, h["content-type"]), (200, "text/javascript; charset=utf-8"))
        self.assertIn("immutable", h["cache-control"])
        st, h, _ = raw_req(port, "GET", "/assets/app-1234.css")
        self.assertEqual(h["content-type"], "text/css; charset=utf-8")
        st, _h, body = raw_req(port, "GET", "/some/route")    # SPA fallback
        self.assertEqual((st, body[:9]), (200, b"<!doctype"))
        st, _h, _ = raw_req(port, "GET", "/assets/missing.js")
        self.assertEqual(st, 404)
        for bad in ("/../secret.txt", "/%2e%2e/secret.txt", "/assets/..%2f..%2fsecret.txt",
                    "/assets/%2e%2e/%2e%2e/secret.txt", "//etc/passwd", "/..%5csecret.txt", "/x%00.js"):
            st, _h, body = raw_req(port, "GET", bad)
            self.assertNotIn(b"secret", body, bad)
            self.assertIn(st, (200, 404), bad)
        st, _h, body = raw_req(port, "GET", "/api/info")   # the API still wins
        self.assertEqual(json.loads(body)["name"], "gemifi builder")

    def test_find_web_dir(self):
        self.assertEqual(self.dashboard.find_web_dir(self.web), self.web)
        with self.assertRaises(SystemExit):
            self.dashboard.find_web_dir(os.path.join(self.tmp, "nope"))

    def test_basic_auth(self):
        import base64
        port = self.start(auth=("gemifi", "s3cret"))
        st, h, body = raw_req(port, "GET", "/api/info")
        self.assertEqual(st, 401)
        self.assertIn("Basic", h["www-authenticate"])
        self.assertIn("error", json.loads(body))
        bad = "Basic " + base64.b64encode(b"gemifi:wrong").decode()
        good = "Basic " + base64.b64encode(b"gemifi:s3cret").decode()
        self.assertEqual(raw_req(port, "GET", "/api/info", headers={"Authorization": bad})[0], 401)
        self.assertEqual(raw_req(port, "GET", "/", headers={"Authorization": bad})[0], 401)
        self.assertEqual(raw_req(port, "GET", "/api/info", headers={"Authorization": good})[0], 200)
        st, _h, _b = raw_req(port, "POST", "/api/validate", {"text": MINIMAL_NUC}, {"Authorization": good})
        self.assertEqual(st, 200)
        self.assertEqual(raw_req(port, "POST", "/api/validate", {"text": MINIMAL_NUC})[0], 401)

    def test_host_check(self):
        HC = self.dashboard.HostCheck
        lo = HC("127.0.0.1", 6055)
        self.assertTrue(lo.ok("127.0.0.1:6055") and lo.ok("localhost:6055") and lo.ok("[::1]:6055"))
        for bad in ("localhost:80", "evil.example:6055", "localhost", "", None):
            self.assertFalse(lo.ok(bad), bad)
        lo2 = HC("127.0.0.1", 6055, ["gemifi.lan"])
        self.assertTrue(lo2.ok("gemifi.lan:443") and lo2.ok("localhost:6055"))
        anyh = HC("0.0.0.0", 6055)
        self.assertTrue(anyh.any and anyh.ok("whatever.example:1") and anyh.ok(None))
        lst = HC("0.0.0.0", 6055, ["Gemifi.lan", "box:8443", "[fd00::1]"])
        self.assertTrue(lst.ok("gemifi.lan:6055") and lst.ok("GEMIFI.LAN") and lst.ok("box:8443"))
        self.assertTrue(lst.ok("[fd00::1]:6055") and lst.ok("localhost:9999"))
        self.assertFalse(lst.ok("box:6055") or lst.ok("evil.example:6055") or lst.ok(None))
        self.assertFalse(HC("::1", 6055).any)
        # over HTTP: the non-loopback rules still keep POSTs JSON-only
        port = self.start(bind="0.0.0.0")
        self.assertEqual(raw_req(port, "GET", "/api/info", headers={"Host": "evil.example"})[0], 200)
        st, _h, _b = raw_req(port, "POST", "/api/validate", {"text": ""},
                             {"Host": "evil.example", "Content-Type": "text/plain"})
        self.assertEqual(st, 415)
        port = self.start(bind="0.0.0.0", allowed_hosts=["gemifi.lan"])
        self.assertEqual(raw_req(port, "GET", "/api/info", headers={"Host": "evil.example"})[0], 403)
        self.assertEqual(raw_req(port, "GET", "/api/info", headers={"Host": "gemifi.lan:6055"})[0], 200)

    def test_cli_env(self):
        """dashboard flags come from GEMIFI_* when not given."""
        import dashboard
        seen = {}
        orig = dashboard.serve
        dashboard.serve = lambda port, user_dir, **kw: seen.update(kw, port=port, user_dir=user_dir) or 0
        env = {"GEMIFI_HOST": "0.0.0.0", "GEMIFI_PORT": "7000", "GEMIFI_CONFIG_DIR": self.tmp,
               "GEMIFI_BUILD_DIR": self.tmp + "/b", "GEMIFI_ALLOWED_HOSTS": "a.lan, b.lan",
               "GEMIFI_PASSWORD": "pw", "GEMIFI_WEB": self.web}
        old = {k: os.environ.get(k) for k in list(env) + ["GEMIFI_USERNAME"]}
        os.environ.update(env)
        os.environ.pop("GEMIFI_USERNAME", None)
        try:
            self.assertEqual(gb.main(["dashboard"]), 0)
            self.assertEqual((seen["host"], seen["port"], seen["user_dir"], seen["build_dir"]),
                             ("0.0.0.0", 7000, self.tmp, self.tmp + "/b"))
            self.assertEqual(seen["allowed_hosts"], ["a.lan", "b.lan"])
            self.assertEqual((seen["auth"], seen["web_dir"]), (("gemifi", "pw"), self.web))
            gb.main(["dashboard", "--port", "7001", "--allowed-host", "c.lan", "--host", "127.0.0.1"])
            self.assertEqual((seen["port"], seen["allowed_hosts"], seen["host"]), (7001, ["c.lan"], "127.0.0.1"))
        finally:
            dashboard.serve = orig
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v


if __name__ == "__main__":
    unittest.main()
