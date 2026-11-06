import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
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

    def test_encoder_acceleration_compiles(self):
        """gm_enc_accel[] (encoder acceleration:) compiles with the core, with and without twins."""
        dev, diags = load(MINIMAL_NUC + "encoder_settings: { acceleration: medium }\n"
                          "encoders:\n  - { id: e1, pin_a: PB6, pin_b: PB7, cc: 20 }\n"
                          "  - { id: e2, pin_a: PB8, pin_b: PB9, cc: 21, acceleration: off }\n")
        self.assertIsNotNone(dev, [str(d) for d in diags])
        self.compile_with_core(gen_dir(dev, self.tmp))

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
        self.assertIn("hubs=10 (ptr)", out)
        types = re.findall(r" hub \d+ type=(\d+) chain=(\d+) n_sel=(\d+) addr=0x(\w+)", out)
        self.assertEqual(types, [("1", "2", "0", "00"), ("1", "1", "0", "00"), ("2", "1", "0", "00"),
                                 ("2", "1", "0", "00"), ("3", "0", "3", "00"), ("3", "0", "4", "00"),
                                 ("4", "0", "0", "21"), ("5", "0", "0", "38"), ("7", "0", "0", "00"),
                                 ("7", "0", "0", "00")])
        # tm1628: RAM size, intensity, grids; CLK/DIO shared, DIO open-drain (flag 8), own STB
        self.assertIn("#define GM_TM1628_RAM 20", h)
        hub8 = out.split(" hub 8 ")[1].split(" hub 9 ")[0]
        hub9 = out.split(" hub 9 ")[1].split("buttons=")[0]
        self.assertIn("ram=14 bright=2 grids=6", hub8)
        self.assertIn("ram=6 bright=7 grids=0", hub9)
        for hb, stb in ((hub8, 37), (hub9, 0)):
            self.assertIn("data hub=0 flags=8 num=36", hb)
            self.assertIn("clock hub=0 flags=0 num=35", hb)
            self.assertIn(f"latch hub=0 flags=0 num={stb}", hb)
        self.assertIn("pin hub=9 flags=0 num=37", out)
        self.assertIn("pin hub=10 flags=1 num=47", out)
        self.assertIn("pin hub=9 flags=0 num=0\n  pin2 hub=9 flags=0 num=1\n  pin3 hub=9 flags=0 num=8", out)
        self.assertIn("pin2 hub=7 flags=0 num=15\n  pin3 hub=255", out)   # bicolour: no pin3
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
        self.assertIn("pads=3 channel=10 curve=1 aftertouch=1 threshold=300 release=150 full=3500 window=2 "
                      "retrigger=30", out)
        self.assertIn("pad hub=6 flags=1 num=2\n  name=Snare\n  note=38", out)
        self.assertIn("pad hub=5 flags=0 num=1\n  name=Pad 3\n  note=42", out)

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
        self.assertIn("pads=0\n", out)
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

    def test_rgb_serial(self):
        stm = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB")

        def dev(hub="tx_pin: PA2, count: 4", leds=""):
            return (stm + f"rgb_serial:\n  - {{ id: p, {hub} }}\n"
                    "leds:\n  - { id: l0, pin: { rgb_serial: p, number: 0 }, pin2: { rgb_serial: p, number: 1 }, "
                    "pin3: { rgb_serial: p, number: 2 } }\n" + leds)

        self.assertClean(dev())
        # pin = LED * 3 + colour, up to count * 3 - 1
        self.assertClean(dev(leds="  - { id: l1, pin: { rgb_serial: p, number: 11 } }\n"))
        self.assertError(dev(leds="  - { id: l1, pin: { rgb_serial: p, number: 12 } }\n"),
                         r"pin 12 is out of range for hub 'p' \(rgb_serial \(4 LEDs x 3; pin = LED \* 3 \+ colour")
        self.assertError(dev(hub="tx_pin: PA2, count: 33"), r"33 is out of range \(1\.\.32\)")
        self.assertError(dev(hub="tx_pin: PA3"), r"only PA2 \(USART2 TX\) is supported")
        self.assertError(dev(hub="count: 4"), r"is required", path="rgb_serial[0].tx_pin")
        self.assertError(dev().replace("pin: PA1", "pin: { rgb_serial: p, number: 5 }"),
                         r"rgb_serial, which can't be an input")
        self.assertError(MINIMAL_NUC + "rgb_serial:\n  - { id: p, tx_pin: PA2 }\n", r"isn't supported on nuvoton")
        # its RAM goes in with the TM1628s'; 16 LEDs default = 6 bytes
        dev_, diags = load(dev(hub="tx_pin: PA2"))
        self.assertIsNotNone(dev_, [str(d) for d in diags])
        files = gd.generate(dev_, "t.yaml")
        self.assertIn("#define GM_TM1628_RAM 6 ", files["gm_device_config.h"])
        self.assertIn(".type = GM_HUB_RGB_SERIAL, .chain = 16,", files["gm_device_config.c"])
        self.assertIn(".ram_bytes = 6,", files["gm_device_config.c"])

    def test_analog_mode(self):
        base = MINIMAL_NUC + "analog:\n"
        self.assertClean(base + "  - { id: w, pin: PA0, mode: pitch_bend }\n")   # no cc: needed
        self.assertClean(base + "  - { id: w, pin: PA0, mode: cc, cc: 1 }\n")
        self.assertError(base + "  - { id: w, pin: PA0, mode: cc }\n", r"is required", path="analog[0].cc")
        self.assertError(base + "  - { id: w, pin: PA0, mode: bend }\n", r"pitch_bend")
        self.assertClean(base + "  - { id: at, pin: PA0, mode: channel_pressure }\n")   # no cc: either
        dev, diags = load(base + "  - { id: m, pin: PA0, cc: 1 }\n  - { id: w, pin: PA2, mode: pitch_bend }\n")
        self.assertIsNotNone(dev, [str(d) for d in diags])
        c = gd.generate(dev, "t.yaml")["gm_device_config.c"]
        self.assertIn("gm_analog_mode_defaults[GM_N_ANALOGS] = { 0, 1 }; /* cc, pitch_bend */", c)
        dev, _ = load(MINIMAL_NUC)
        c = gd.generate(dev, "t.yaml")["gm_device_config.c"]
        self.assertIn("gm_analog_mode_defaults[1] = { 0 };", c)

    C61_DIN_DMA = ("  mode: usb_merge\n", "  mode: usb_merge\n  dma: true\n")
    C61_NO_PRE = ("    row_time: 1.5us\n    settle: 1us\n    precharge: true\n",
                  "    row_time: 3.5us\n    settle: 3us\n")

    def c61(self):
        with open(os.path.join(REPO, "devices", "m-audio-code61.yaml"), encoding="utf-8") as f:
            return f.read()

    def test_dma_alloc(self):
        """stm32f1: each DMA user gets its channels (RM0008 table 78); a clash is an error on the
        optional user, and the map goes in the header."""
        c61 = self.c61()
        d, diags = load(c61)
        self.assertIsNotNone(d, [str(x) for x in diags])
        # the Code 61's original map: matrix on TIM1, analog on TIM4, tick on TIM2
        self.assertEqual(d.dma_map, [(1, "ADC1", "hardware_scan.analog"),
                                     (2, "TIM1_CH1", "hardware_scan.matrix columns in"),
                                     (3, "TIM1_CH2", "hardware_scan.matrix read"),
                                     (4, "TIM4_CH2", "hardware_scan.analog mux step"),
                                     (5, "TIM1_UP", "hardware_scan.matrix select"),
                                     (6, "TIM1_CH3", "hardware_scan.matrix precharge"),
                                     (7, "USART2_TX", "rgb_serial 'pad_leds'")])
        self.assertEqual(d.scan_timers, {"matrix": (1, {"sel": 0, "in": 1, "rd": 2, "pre": 3}),
                                         "analog": (4, {"mux": 2}), "tick": 2})
        self.assertEqual(gd.dma_text(d).split("; ")[-1], "free: none")
        h = gd.generate(d, "t.yaml")["gm_device_config.h"]
        self.assertIn("/* DMA1 channels: 1 ADC1 (hardware_scan.analog), 2 TIM1_CH1 (hardware_scan.matrix", h)
        self.assertIn("/* timers: TIM1 hardware_scan.matrix, TIM2 scan tick, TIM4 hardware_scan.analog", h)
        for line in ("GM_HW_SCAN_TIM 1 ", "GM_HW_SCAN_SEL 0 ", "GM_HW_SCAN_SEL_CH 5\n", "GM_HW_SCAN_IN 1 ",
                     "GM_HW_SCAN_IN_CH 2\n", "GM_HW_SCAN_RD 2 ", "GM_HW_SCAN_RD_CH 3\n", "GM_HW_SCAN_PRE 3 ",
                     "GM_HW_SCAN_PRE_CH 6\n", "GM_HW_ADC_TIM 4 ", "GM_HW_ADC_MUX 2 ", "GM_HW_ADC_MUX_CH 4\n",
                     "GM_SCAN_TICK_TIM 2 "):
            self.assertIn("#define " + line, h)
        self.assertIn("#define GM_DIN_DMA 0 ", h)
        self.assertIn(".ram_bytes = 6, .dma = 1,", gd.generate(d, "t.yaml")["gm_device_config.c"])
        # DIN by DMA: with precharge the scans need 6 channels (ADC1, mux, 4 matrix steps), DIN 2:
        # no timer choice fits; the error says what would
        din_dma = c61.replace(*self.C61_DIN_DMA)
        e = self.assertError(din_dma, r"needs DMA1 channel 4 \(USART1_TX\), which hardware_scan.analog uses "
                                      r"\(TIM4_CH2\): set din_midi dma: false", path="din_midi.dma")
        self.assertIn("or turn hardware_scan.matrix precharge off: the scan then needs 2 channels", e.msg)
        self.assertError(din_dma, r"needs DMA1 channel 5 \(USART1_RX\), which hardware_scan.matrix uses")
        # ... the pad LEDs giving channel 7 back doesn't make it fit
        self.assertError(din_dma.replace("    count: 16\n", "    count: 16\n    dma: false\n", 1),
                         r"needs DMA1 channel 4 .*precharge off")
        # ... fine without the scans
        no_scan = din_dma.replace("    precharge: true\n  analog: true\n", "    precharge: true\n  analog: false\n")
        no_scan = no_scan.replace("hardware_scan:\n  matrix:\n    row_time: 1.5us\n    settle: 1us\n    precharge: true\n",
                                  "hardware_scan:\n")
        d, diags = load(no_scan.replace("    count: 16\n", "    count: 16\n    dma: false\n", 1))
        self.assertIsNotNone(d, [str(x) for x in diags if x.level == "error"])
        self.assertEqual(d.dma_map, [(4, "USART1_TX", "din_midi"), (5, "USART1_RX", "din_midi")])
        self.assertEqual(gd.dma_text(d).split("; ")[-1], "free: 1, 2, 3, 6, 7")
        self.assertEqual(d.scan_timers, {"tick": 2})
        files = gd.generate(d, "t.yaml")
        self.assertIn("#define GM_DIN_DMA 1 ", files["gm_device_config.h"])
        self.assertNotIn("GM_HW_SCAN_TIM", files["gm_device_config.h"])
        self.assertIn(".ram_bytes = 6, .dma = 0,", files["gm_device_config.c"])
        # the matrix scan without precharge leaves 2 and 6
        d, _ = load(c61.replace("    precharge: true\n", ""))
        self.assertEqual([ch for ch, _r, _u in d.dma_map], [1, 3, 4, 5, 7])
        self.assertIn("#define GM_HW_SCAN_PRE_CH 0\n", gd.generate(d, "t.yaml")["gm_device_config.h"])
        # other platforms: no map
        self.assertEqual(gd.dma_text(load(MINIMAL_NUC)[0]), "")
        self.assertEqual(gd.timers_text(load(MINIMAL_NUC)[0]), "")

    def test_dma_auto_moves_scans(self):
        """timer: auto (the default) moves the scans off channels a dma: user needs."""
        c61 = self.c61().replace(*self.C61_DIN_DMA).replace(*self.C61_NO_PRE)
        d, diags = load(c61)
        self.assertIsNotNone(d, [str(x) for x in diags if x.level == "error"])
        # DIN by DMA, both scans, pad LEDs by DMA: all 7 channels
        self.assertEqual(d.dma_map, [(1, "ADC1", "hardware_scan.analog"),
                                     (2, "TIM1_CH1", "hardware_scan.matrix select"),
                                     (3, "TIM1_CH2", "hardware_scan.matrix read"),
                                     (4, "USART1_TX", "din_midi"), (5, "USART1_RX", "din_midi"),
                                     (6, "TIM3_CH1", "hardware_scan.analog mux step"),
                                     (7, "USART2_TX", "rgb_serial 'pad_leds'")])
        self.assertEqual(d.scan_timers, {"matrix": (1, {"sel": 1, "rd": 2}), "analog": (3, {"mux": 1}),
                                         "tick": 2})
        h = gd.generate(d, "t.yaml")["gm_device_config.h"]
        self.assertIn("#define GM_HW_SCAN_SEL 1 /* TIM1_CH1: select */", h)
        self.assertIn("#define GM_HW_ADC_TIM 3 /* hardware_scan.analog on TIM3: ADC1 by TIM3_TRGO (OC2REF) */", h)
        self.assertIn("TIM3 hardware_scan.analog (ADC1 started by TIM3_TRGO (OC2REF))", gd.timers_text(d))
        # an explicit analog timer that takes channel 4 can't: the error points at auto
        e = self.assertError(c61.replace("  analog: true\n", "  analog: { timer: tim4 }\n"),
                             r"needs DMA1 channel 4 \(USART1_TX\), which hardware_scan.analog uses", path="din_midi.dma")
        self.assertIn("set the hardware_scan timers to auto (it finds: matrix on TIM1, analog on TIM3)", e.msg)

    def test_dma_explicit_timers(self):
        c61 = self.c61()
        # the analog scan on TIM2: the matrix moves its precharge to CC4 (channel 5 is TIM2_CH1's),
        # and the scan tick to TIM3
        d, diags = load(c61.replace("  analog: true\n", "  analog: { timer: TIM2 }\n"))
        self.assertIsNotNone(d, [str(x) for x in diags if x.level == "error"])
        self.assertEqual(d.scan_timers, {"matrix": (1, {"sel": 1, "in": 3, "rd": 2, "pre": 4}),
                                         "analog": (2, {"mux": 1}), "tick": 3})
        self.assertIn("#define GM_SCAN_TICK_TIM 3 ", gd.generate(d, "t.yaml")["gm_device_config.h"])
        # the matrix on TIM3 (no precharge): UP + CC1
        no_pre = c61.replace(*self.C61_NO_PRE)
        d, _ = load(no_pre.replace("    settle: 3us\n", "    settle: 3us\n    timer: tim3\n"))
        self.assertEqual(d.scan_timers["matrix"], (3, {"sel": 0, "rd": 1}))
        self.assertEqual(d.scan_timers["analog"], (4, {"mux": 2}))
        # TIM3 has 3 DMA channels, TIM4 lands on ADC1's: no precharge scan on either
        pre_on = c61.replace("    precharge: true\n", "    precharge: true\n    timer: tim3\n")
        timer_line = pre_on.splitlines().index("    timer: tim3") + 1
        e = self.assertError(pre_on, r"TIM3 can't pace the matrix scan: it needs 4 DMA requests",
                             path="hardware_scan.matrix.timer", line=timer_line)
        self.assertIn("TIM3 has UP 3, CH1 6, CH3 2, CH4 3 (two share channel 3)", e.msg)
        self.assertIn("(timer: auto picks matrix on TIM1, analog on TIM4)", e.msg)
        e = self.assertError(c61.replace("    precharge: true\n", "    precharge: true\n    timer: tim4\n"),
                             r"TIM4 can't pace the matrix scan")
        self.assertIn("channel 1 is ADC1's, for hardware_scan.analog", e.msg)
        # one timer for both
        self.assertError(no_pre.replace("    settle: 3us\n", "    settle: 3us\n    timer: tim3\n")
                         .replace("  analog: true\n", "  analog: { timer: tim3 }\n"),
                         r"TIM3 is hardware_scan.matrix's timer too", path="hardware_scan.analog.timer")
        # names
        self.assertError(c61.replace("  analog: true\n", "  analog: { timer: tim1 }\n"),
                         r"the analog scan can't use TIM1")
        self.assertError(c61.replace("  analog: true\n", "  analog: { timer: tim9 }\n"),
                         r"unknown timer 'tim9' \(did you mean 'tim4'\? valid: auto, tim2, tim3, tim4\)")
        self.assertError(c61.replace("  analog: true\n", "  analog: { timr: tim3 }\n"), r"unknown key")
        d, _ = load(c61.replace("    precharge: true\n", "    precharge: true\n    timer: auto\n")
                    .replace("  analog: true\n", "  analog: { timer: auto }\n"))
        self.assertEqual(d.scan_timers["matrix"][0], 1)

    def test_dma_options(self):
        """Every matrix option: one request per step, the update only for the select, channels apart."""
        d, _ = load(self.c61())
        for t in (1, 2, 3, 4):
            for pre in (False, True):
                opts = gd.scan_options(d, "matrix", t, pre)
                for _s, _t, roles, claims in opts:
                    self.assertEqual(len({ch for ch, _r, _x in claims}), len(claims))
                    self.assertTrue(all(q != 0 for r, q in roles.items() if r != "sel"))
                self.assertEqual(bool(opts), t != 3 or not pre, (t, pre))   # TIM3: 3 channels only
        self.assertEqual([o[3][1][0] for o in gd.scan_options(d, "analog", 3)], [6, 2, 3])

    @unittest.skipUnless(shutil.which("arm-none-eabi-gcc"), "needs arm-none-eabi-gcc")
    def test_dma_map_compiles(self):
        """The platform's static checks (scan_dma.c, adc_dma.c, hal.c, main.c) take what the
        allocator picks."""
        c61 = self.c61()
        no_pre = c61.replace(*self.C61_NO_PRE)
        variants = [c61, c61.replace(*self.C61_DIN_DMA).replace(*self.C61_NO_PRE),
                    c61.replace("  analog: true\n", "  analog: { timer: tim2 }\n"),
                    no_pre.replace("    settle: 3us\n", "    settle: 3us\n    timer: tim3\n"),
                    no_pre.replace("    settle: 3us\n", "    settle: 3us\n    timer: tim2\n")
                    .replace("  analog: true\n", "  analog: { timer: tim3 }\n")]
        plat = os.path.join(REPO, "platform", "stm32f1")
        srcs = [os.path.join(plat, "src", f) for f in ("scan_dma.c", "adc_dma.c", "hal.c")]
        srcs.append(os.path.join(REPO, "firmware", "stm32f1", "midi-controller", "main.c"))
        for i, text in enumerate(variants):
            d, diags = load(text)
            self.assertIsNotNone(d, [str(x) for x in diags if x.level == "error"])
            out = tempfile.mkdtemp()
            self.addCleanup(shutil.rmtree, out)
            for name, body in gd.generate(d, "t.yaml").items():
                with open(os.path.join(out, name), "w", encoding="utf-8") as f:
                    f.write(body)
            r = subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m3", "-mthumb", "-std=gnu11", "-Wall",
                                "-Wextra", "-Wno-unused-parameter", "-Werror", "-fsyntax-only",
                                "-I" + out, "-I" + os.path.join(plat, "include"), "-I" + CORE_INC,
                                "-I" + os.path.join(plat, "boards"), '-DBOARD_HEADER="m-audio-code61.h"',
                                "-DFW_VERSION_MAJOR=0", "-DFW_VERSION_MINOR=0", "-DFW_VERSION_PATCH=0", *srcs],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f"variant {i}: {r.stderr}")

    def test_din_midi(self):
        stm = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB")

        def dev(body="tx_pin: PA9, rx_pin: PA10", more=""):
            return stm + f"din_midi: {{ {body} }}\n" + more

        def warns(text):
            return [str(d) for d in self.errs(text, level="warning")]

        self.assertClean(dev())
        self.assertClean(dev("tx_pin: PA9"))                                   # MIDI Out only
        for m in ("separate_port", "usb_merge", "din_only"):
            self.assertClean(dev(f"tx_pin: PA9, rx_pin: PA10, mode: {m}"))
        self.assertError(dev("tx_pin: PA9, mode: thru"), r"separate_port")
        self.assertError(dev("rx_pin: PA10"), r"is required", path="din_midi.tx_pin")
        self.assertError(dev("tx_pin: PA2"), r"USART1: tx_pin PA9, rx_pin PA10")
        self.assertError(dev("tx_pin: PA9, rx_pin: PA3"), r"USART1: tx_pin PA9, rx_pin PA10")
        self.assertError(MINIMAL_NUC + "din_midi: { tx_pin: PA2 }\n", r"din_midi isn't supported on nuvoton")
        self.assertError(dev().replace("pin: PA1", "pin: PA9"), r"PA9")              # pin used twice
        self.assertTrue(any("ignored: only usb_merge" in w
                            for w in warns(dev("tx_pin: PA9, mode: separate_port, usb_to_din: true"))))
        # usb_merge is the default; separate_port warns (two ports hang Chrome's Web MIDI on Windows)
        self.assertEqual(warns(dev()), [])
        self.assertTrue(any("hang" in w for w in warns(dev("tx_pin: PA9, mode: separate_port"))))
        self.assertTrue(any("no rx_pin" in w for w in warns(dev("tx_pin: PA9, thru: true"))))
        self.assertTrue(any("only separate_port has its own port" in w
                            for w in warns(dev("tx_pin: PA9, mode: din_only, port_name: X"))))
        # separate_port needs cable 1, which a DAW port (mcu:) has
        self.assertError(dev("tx_pin: PA9, mode: separate_port", more="mcu: {}\n"),
                         r"separate_port needs the second USB port")
        self.assertClean(dev(more="mcu: {}\n"))
        # the defines and params
        d, diags = load(dev("tx_pin: PA9, rx_pin: PA10, mode: separate_port, thru: true"))
        self.assertIsNotNone(d, [str(x) for x in diags])
        h = gd.generate(d, "t.yaml")["gm_device_config.h"]
        for line in ("#define GM_DIN 1 ", "#define GM_DIN_MODE 1 /* separate_port */", "#define GM_DIN_TX_PIN 9 ",
                     "#define GM_DIN_RX_PIN 10 ", "#define GM_DIN_THRU_DEFAULT 1", "#define GM_DIN_OUT_DEFAULT 1",
                     "#define GM_DIN_CABLE 1", '#define GM_DIN_PORT_NAME "test MIDI"'):
            self.assertIn(line, h)
        self.assertEqual(gd.stored_counts(d)[1] - gd.stored_counts(load(stm)[0])[1], 2)   # thru, din_out
        d, _ = load(dev("tx_pin: PA9, mode: usb_merge, usb_to_din: true"))
        h = gd.generate(d, "t.yaml")["gm_device_config.h"]
        self.assertIn("#define GM_DIN_RX_PIN 65535 /* no MIDI In */", h)
        self.assertIn("#define GM_DIN_USB_DEFAULT 1", h)
        self.assertNotIn("GM_DIN_CABLE", h)
        self.assertEqual(gd.stored_counts(d)[1] - gd.stored_counts(load(stm)[0])[1], 3)   # + usb_to_din
        self.assertIn("#define GM_DIN 0 ", gd.generate(load(stm)[0], "t.yaml")["gm_device_config.h"])

    def test_xy_pad(self):
        stm = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB")

        def dev(hub="x_plus_pin: PB1, x_minus_pin: PC5, y_plus_pin: PC4, y_minus_pin: PB10",
                x="{ xy_pad: xy, number: x }", y="{ xy_pad: xy, number: y }", extra=""):
            return (stm + f"xy_pad:\n  - {{ id: xy, {hub} }}\nanalog:\n"
                    f"  - {{ id: ax, pin: {x}, cc: 16{extra} }}\n  - {{ id: ay, pin: {y}, cc: 17 }}\n")

        self.assertClean(dev())
        self.assertClean(dev(x="{ xy_pad: xy, number: 0 }", y="{ xy_pad: xy, number: 1 }"))
        self.assertError(dev(x="{ xy_pad: xy, number: z }"), r"'z' is not a pin of hub 'xy' \(use x or y\)")
        self.assertError(dev(x="{ xy_pad: xy, number: 2 }"), r"channel 2 is out of range|pin 2 is out of range")
        self.assertError(dev(extra=", mode: pitch_bend"), r"XY pad reads nothing until touched")
        self.assertClean(dev(extra=", mode: channel_pressure"))
        self.assertError(dev(hub="x_plus_pin: PB1, x_minus_pin: PC5, y_plus_pin: PC4"), r"is required",
                         path="xy_pad[0].y_minus_pin")
        self.assertError(dev(hub="x_plus_pin: PB1, x_minus_pin: PC5, y_plus_pin: PB11, y_minus_pin: PB10"),
                         r"X is read on y_minus_pin or y_plus_pin: neither has an ADC")
        self.assertError(dev(hub="x_plus_pin: PB1, x_minus_pin: PC5, y_plus_pin: PC4, y_minus_pin: PB10, "
                                 "touch_min: 3000, touch_max: 2000"), r"touch_min 3000 must be below")
        self.assertError(dev(hub="x_plus_pin: PB1, x_minus_pin: PA0, y_plus_pin: PC4, y_minus_pin: PB10")
                         .replace("pin: PA1", "pin: PA0"), r"PA0")                           # pin used twice
        # a pad is read on a mux or an ADC pin, not an XY pad
        self.assertError(dev() + "pads:\n  - { id: p, pin: { xy_pad: xy, number: x }, note: 36 }\n",
                         r"xy_pad, which can't be an analog input")
        # Each axis is read on the other plate's minus pin when it has an ADC, else its plus pin.
        d, diags = load(dev())
        self.assertIsNotNone(d, [str(x) for x in diags])
        c = gd.generate(d, "t.yaml")["gm_device_config.c"]
        self.assertIn(".type = GM_HUB_XY,", c)
        self.assertIn(".data = { 0, 0, 36 }, .clock = { 0, 0, 37 },", c)    # X on PC4 (Y+), Y on PC5 (X-)
        self.assertIn(".sel = { { 0, 0, 17 }, { 0, 0, 37 }, { 0, 0, 36 }, { 0, 0, 26 } },", c)
        self.assertIn(".touch_min = 570, .touch_max = 3430,", c)
        self.assertIn("{ 1, 0, 0 }", c)                                     # ax: hub 1, channel 0 (x)
        self.assertIn("{ 1, 0, 1 }", c)

    def test_displays(self):
        stm = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB")
        tm = ("tm1628:\n  - { id: ta, clk_pin: PC10, dio_pin: { number: PC11, mode: OUTPUT_OPEN_DRAIN }, "
              "stb_pin: PB12 }\n")

        def dev(chars="[{ tm1628: ta, number: 48 }, { tm1628: ta, number: 0 }]", extra="", more=""):
            return stm + tm + f"displays:\n  - {{ id: disp, chars: {chars}{extra} }}\n" + more

        self.assertClean(dev())
        self.assertClean(dev(extra=", type: segment14"))
        self.assertError(dev(extra=", type: lcd"), r"unknown display type 'lcd' \(use segment14\)")
        self.assertError(dev(chars="[]"), r"needs chars")
        # 15 pins from each first one: 97..111 fits on 14 bytes, 98 doesn't
        self.assertClean(dev(chars="[{ tm1628: ta, number: 97 }]"))
        self.assertError(dev(chars="[{ tm1628: ta, number: 98 }]"),
                         r"pins 98\.\.112 \(segments a\.\.m and the dot\) don't fit on hub 'ta' \(0\.\.111\)")
        self.assertError(dev(chars="[PB0]"), r"must be a hub pin")
        self.assertError(dev(chars="[{ number: PB0 }]"), r"must be a hub pin")
        # its segments are pins: an LED on one of them is a clash
        self.assertError(dev(more="leds:\n  - { id: l, pin: { tm1628: ta, number: 5 } }\n"),
                         r"ta\.5 is already used by leds\[0\]\.pin", path="displays[0].chars[1]")
        self.assertClean(dev(more="leds:\n  - { id: l, pin: { tm1628: ta, number: 15 } }\n"))
        self.assertError(dev(chars="[" + ", ".join(["{ tm1628: ta, number: 0 }"] * 17) + "]"),
                         r"17 characters: at most 16")
        two = dev() + "  - { id: d2, chars: [{ tm1628: ta, number: 96 }] }\n"
        self.assertError(two, r"2 displays: at most 1")
        dev_, diags = load(dev())
        self.assertIsNotNone(dev_, [str(d) for d in diags])
        files = gd.generate(dev_, "t.yaml")
        self.assertIn("#define GM_N_DISPLAYS 1 ", files["gm_device_config.h"])
        c = files["gm_device_config.c"]
        self.assertIn("static const gm_pin_t display_0_chars[2] = {", c)
        self.assertIn("{ GM_DISPLAY_SEG14, 2, display_0_chars }, /* disp */", c)
        dev_, _ = load(stm)
        files = gd.generate(dev_, "t.yaml")
        self.assertIn("#define GM_N_DISPLAYS 0 ", files["gm_device_config.h"])
        self.assertNotIn("gm_displays", files["gm_device_config.c"])

    def test_tm1628(self):
        stm = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB")
        share = "{ number: %s, allow_other_uses: true }"
        dio = "{ number: PC11, mode: OUTPUT_OPEN_DRAIN, allow_other_uses: true }"

        def dev(extra_a="", extra_b="", stb_b="PC12", leds="", clk=share % "PC10"):
            return (stm + "tm1628:\n"
                    f"  - {{ id: ta, clk_pin: {clk}, dio_pin: {dio}, stb_pin: PB12{extra_a} }}\n"
                    f"  - {{ id: tb, clk_pin: {share % 'PC10'}, dio_pin: {dio}, stb_pin: {stb_b}{extra_b} }}\n"
                    "leds:\n  - { id: la, pin: { tm1628: ta, number: 111 } }\n"
                    "  - { id: lb, pin: { tm1628: tb, number: 47, inverted: true } }\n" + leds)

        self.assertClean(dev())
        # pin = byte * 8 + bit, up to ram_bytes * 8 - 1
        self.assertError(dev(leds="  - { id: lc, pin: { tm1628: ta, number: 112 } }\n"),
                         r"pin 112 is out of range for hub 'ta' \(tm1628 \(14 RAM bytes x 8; pin = byte \* 8 "
                         r"\+ bit\): 0\.\.111\)")
        self.assertClean(dev(extra_b=", ram_bytes: 16", leds="  - { id: lc, pin: { tm1628: tb, number: 127 } }\n"))
        self.assertError(dev(extra_b=", ram_bytes: 5"), r"pin 47 is out of range for hub 'tb'")
        self.assertError(dev(extra_a=", ram_bytes: 17"), r"17 is out of range \(1\.\.16\)")
        self.assertError(dev(extra_a=", intensity: 8"), r"8 is out of range \(0\.\.7\)")
        self.assertError(dev(extra_a=", grids: 3"), r"3 is out of range \(4\.\.7\)")
        self.assertClean(dev(extra_a=", intensity: 0, grids: 7"))
        # outputs only
        self.assertError(dev().replace("pin: PA1", "pin: { tm1628: ta, number: 0 }"),
                         r"tm1628, which can't be an input")
        # CLK/DIO shared only with allow_other_uses; STB never
        self.assertError(dev(clk="PC10"), r"PC10 is already used")
        self.assertError(dev(stb_b="PB12"), r"PB12 is already used")
        self.assertError(dev(stb_b=share % "PB12").replace("stb_pin: PB12", "stb_pin: " + share % "PB12"),
                         r"each needs its own stb_pin", path="tm1628[1].stb_pin")
        # a TM1628 line can't double as a shift register's clock
        self.assertError(dev() + f"sn74hc595:\n  - {{ id: s, data_pin: PB0, clock_pin: {share % 'PC10'}, "
                                 "latch_pin: PB1 }\n", r"PC10 is already used.*same control line")
        # open-drain only on dio_pin
        self.assertError(dev(clk="{ number: PC10, mode: OUTPUT_OPEN_DRAIN, allow_other_uses: true }"),
                         r"open_drain is only supported on a tm1628 dio_pin")
        self.assertError(dev().replace("mode: OUTPUT_OPEN_DRAIN", "mode: INPUT"),
                         r"use mode: OUTPUT or OUTPUT_OPEN_DRAIN")
        # RGB LED: pin red, pin2 green, pin3 blue; pin3 needs pin2
        rgb = "  - { id: rgb, pin: { tm1628: ta, number: 0 }, pin2: { tm1628: ta, number: 1 }%s }\n"
        self.assertClean(dev(leds=rgb % ", pin3: { tm1628: ta, number: 2 }"))
        self.assertError(dev(leds=rgb.replace(", pin2: { tm1628: ta, number: 1 }", "") %
                             ", pin3: { tm1628: ta, number: 2 }"), r"an RGB LED needs pin2 too")
        self.assertError(dev(leds=rgb % ", pin3: { tm1628: ta, number: 1 }"), r"ta\.1 is already used")

    def test_led_looks(self):
        leds = ("leds:\n"
                "  - { id: s, pin: PB0%s }\n"
                "  - { id: bi, pin: PB1, pin2: PB2%s }\n"
                "  - { id: rgb, pin: PB3, pin2: PB4, pin3: PB5%s }\n")

        def looks(s="", bi="", rgb="", top=""):
            dev, diags = load(MINIMAL_NUC + top + leds % (s, bi, rgb))
            self.assertEqual(gd.errors(diags), [], [str(d) for d in diags])
            return [x.looks for x in dev.leds]

        self.assertEqual(looks(), [None, None, None])        # defaults: generated as 0
        # idle: on (unquoted YAML reads it as a boolean) with no active: dark when active
        self.assertEqual(looks(s=", idle: on"), [(1, 0), None, None])
        self.assertEqual(looks(s=", idle: 'on', active: off"), [(1, 0), None, None])
        self.assertEqual(looks(bi=", idle: colour2"), [None, (2, 1), None])
        self.assertEqual(looks(bi=", idle: colour1"), [None, (1, 0), None])
        self.assertEqual(looks(bi=", idle: both, active: colour2"), [None, (3, 2), None])
        self.assertEqual(looks(rgb=", idle: blue, active: white"), [None, None, (4, 7)])
        self.assertEqual(looks(rgb=", idle: cyan"), [None, None, (6, 7)])
        self.assertEqual(looks(rgb=", idle: magenta, active: on"), [None, None, (5, 7)])
        # led_defaults: every LED, nearest look it can show; the LED's own fields win
        self.assertEqual(looks(top="led_defaults: { idle: blue, active: white }\n"),
                         [(1, 0), (1, 3), (4, 7)])   # single: on / off; bicolour: colour1 / both
        self.assertEqual(looks(top="led_defaults: { idle: yellow }\n"), [(1, 0), (3, 1), (3, 7)])
        self.assertEqual(looks(top="led_defaults: { idle: green }\n", rgb=", idle: off"),
                         [(1, 0), (2, 1), (0, 7)])
        # errors: a look the LED can't show, an unknown name
        self.assertError(MINIMAL_NUC + leds % (", idle: blue", "", ""),
                         r"'blue' is a look for an RGB LED; a single-colour LED \(pin only\) takes off, on",
                         path="leds[0].idle")
        self.assertError(MINIMAL_NUC + leds % ("", "", ", active: colour1"),
                         r"'colour1' is a look for a bicolour LED; an RGB LED.*takes off, on, red")
        self.assertError(MINIMAL_NUC + leds % ("", ", idle: colour3", ""),
                         r"unknown look 'colour3' \(did you mean 'colour\d'\?\)", path="leds[1].idle")
        self.assertError(MINIMAL_NUC + "led_defaults: { idle: purple }\n" + leds % ("", "", ""),
                         r"unknown look 'purple'.*valid: on, off", path="led_defaults.idle")
        self.assertError(MINIMAL_NUC + "led_defaults: { idle: blue, dim: true }\n" + leds % ("", "", ""),
                         r"unknown key \(known: idle, active\)", path="led_defaults.dim")
        # the same look twice, given: a warning
        warns = [str(d) for d in self.errs(MINIMAL_NUC + leds % (", idle: on, active: on", "", ""), level="warning")]
        self.assertTrue(any("never shows its state" in w for w in warns), warns)
        # generated: GM_LED_LOOKS(idle, active), 0 for the defaults
        dev, _ = load(MINIMAL_NUC + leds % (", idle: on", "", ", idle: blue, active: white"))
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(gen_dir(dev, tmp), "gm_device_config.c")) as f:
                c = f.read()
        self.assertIn("GM_NO_PIN, GM_NO_PIN, GM_LED_LOOKS(1, 0), 0, 0 }, /* s: PB0 */", c)
        self.assertRegex(c, r"GM_NO_PIN, 0, 0, 0 \}, /\* bi: PB1 \*/")
        self.assertIn("GM_LED_LOOKS(4, 7), 0, 0 }, /* rgb: PB3 */", c)

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

    def test_stm32f1_pins(self):
        stm = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RC")
        self.assertClean(stm)
        self.assertClean(stm.replace("pin: PA1", "pin: PG15"))
        self.assertError(stm.replace("pin: PA1", "pin: PH0"), r"'PH0' is not a pin on stm32f1")
        self.assertError(stm.replace("pin: PA1", "pin: PA12"), r"PA12 is a USB data line")
        self.assertClean(stm + "analog:\n  - { id: v, pin: PC5, cc: 7 }\n")
        self.assertError(stm + "analog:\n  - { id: v, pin: PC6, cc: 7 }\n",
                         r"PC6 has no ADC on stm32f1 \(ADC pins: PA0-PA7, PB0-PB1, PC0-PC5")
        warns = [str(d) for d in self.errs(stm.replace("pin: PA1", "pin: PA13"), level="warning")]
        self.assertTrue(any("SWD" in w for w in warns), warns)
        warns = [str(d) for d in self.errs(stm.replace("STM32F103RC", "NUC120"), level="warning")]
        self.assertTrue(any("STM32F1-class" in w for w in warns), warns)
        self.assertEqual(self.errs(stm.replace("STM32F103RC", "GD32F103RC"), level="warning"), [])

    def test_hardware_scan(self):
        """hardware_scan: (core/README.md "Hardware scan"): stm32f1 only; matrix: needs a matrix and
        a row that fits the settle plus the DMA steps' gaps; analog: needs something to read."""
        stm = (MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB") +
               "switch_matrix:\n  - { id: sm, rows: [PA2, PA3], columns: [PB0, PB1] }\n")
        hs = "hardware_scan:\n  matrix: { row_time: %s, settle: %s%s }\n"
        dev, diags = load(stm + hs % ("3.5us", "3us", ""))
        self.assertEqual([str(d) for d in diags if d.level == "error"], [])
        self.assertEqual((dev.hw_scan, dev.hw_adc), ((3500, 3000, False), False))
        dev, _ = load(stm + hs % ("1.5us", "1us", ", precharge: true"))
        self.assertEqual(dev.hw_scan, (1500, 1000, True))
        h = gd.generate(dev, "t.yaml")["gm_device_config.h"]
        for want in ("#define GM_HW_SCAN 1", "#define GM_HW_SCAN_ROW_NS 1500", "#define GM_HW_ADC 0",
                     "#define GM_HW_SCAN_SETTLE_NS 1000", "#define GM_HW_SCAN_PRECHARGE 1"):
            self.assertIn(want, h)
        dev, _ = load(stm)
        self.assertIn("#define GM_HW_SCAN 0", gd.generate(dev, "t.yaml")["gm_device_config.h"])
        self.assertClean(stm + hs % ("750ns", "500ns", ""))
        self.assertError(stm + hs % ("3.2us", "3us", ""), r"leaves no room after the 3us settle: at least 3.25us",
                         path="hardware_scan.matrix.row_time")
        self.assertError(stm + hs % ("1.2us", "1us", ", precharge: true"), r"at least 1.5us with precharge")
        self.assertError(stm + hs % ("2us", "0.4us", ", precharge: true"), r"too short with precharge",
                         path="hardware_scan.matrix.settle")
        self.assertError(stm + "hardware_scan:\n  matrix: { settle: 3us }\n", r"is required",
                         path="hardware_scan.matrix.row_time")
        self.assertError(stm + hs % ("3.5", "3us", ""), r"needs a unit")
        self.assertError(stm + "hardware_scan:\n  matrix: { row_time: 4us, settle: 3us, rows: 2 }\n",
                         r"unknown key")
        self.assertError(stm + "hardware_scan: { row_time: 4us }\n", r"unknown key")
        self.assertError(MINIMAL_NUC + "switch_matrix:\n  - { id: sm, rows: [PA2, PA3], columns: [PB0, PB1] }\n" +
                         hs % ("3.5us", "3us", ""), r"not available on nuvoton-nuc1xx")
        stm0 = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB")
        self.assertError(stm0 + hs % ("3.5us", "3us", ""), r"needs a key_matrix: or switch_matrix:")
        # analog: true
        self.assertError(stm0 + "hardware_scan: { analog: true }\n", r"needs analog: inputs or pads:")
        dev, _ = load(stm0 + "analog:\n  - { id: v, pin: PA0, cc: 7 }\nhardware_scan: { analog: true }\n")
        self.assertEqual((dev.hw_scan, dev.hw_adc), (None, True))
        self.assertIn("#define GM_HW_ADC 1", gd.generate(dev, "t.yaml")["gm_device_config.h"])

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
        self.assertError(btn % "INPUT_OUTPUT_OPEN_DRAIN", r"open_drain is only supported on a tm1628 dio_pin")
        self.assertError(btn % "{ input: true, pulup: true }", r"unknown key \(did you mean 'pullup'")
        led = MINIMAL_NUC + "leds:\n  - { id: l, pin: { number: PA2, mode: %s } }\n"
        self.assertClean(led % "OUTPUT")
        self.assertError(led % "INPUT_PULLUP", r"this pin is an output: use mode: OUTPUT")
        self.assertError(led % "OUTPUT_OPEN_DRAIN", r"open_drain is only supported on a tm1628 dio_pin")
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

    def test_matrix_scan_sizes(self):
        """GM_MATRIX_ROWS / _COL_RUNS / _DECODER size controller.c's scan tables."""
        def defs(text):
            dev, diags = gd.load_device("x.yaml", text)
            self.assertFalse(gd.errors(diags), diags)
            h = gd.generate(dev, "x.yaml")["gm_device_config.h"]
            return {k: int(re.search(rf"#define {k} (\d+)", h).group(1))
                    for k in ("GM_MATRIX_ROWS", "GM_MATRIX_COL_RUNS", "GM_MATRIX_DECODER")}
        dev, _ = gd.load_device(KEYSTATION)
        h = gd.generate(dev, "x.yaml")["gm_device_config.h"]
        # PB0-PB5, PB8, PB12: 3 runs
        for s in ("#define GM_MATRIX_ROWS 8 ", "#define GM_MATRIX_COL_RUNS 3 ", "#define GM_MATRIX_DECODER 0 "):
            self.assertIn(s, h)
        m = MINIMAL_NUC + "key_matrix:\n  type: switch\n  rows: [PA8, PA9]\n  columns: [%s]\n  keys: %d\n"
        self.assertEqual(defs(m % ("PB0, PB1, PB2, PB3", 8)),
                         {"GM_MATRIX_ROWS": 2, "GM_MATRIX_COL_RUNS": 1, "GM_MATRIX_DECODER": 0})
        # out of order, another port, a gap: each starts a run
        self.assertEqual(defs(m % ("PB1, PB0, PC2, PC3, PC5", 10))["GM_MATRIX_COL_RUNS"], 4)
        # columns past the 16th are read pin by pin: not counted
        cols = ", ".join(f"P{p}{b}" for p in "BC" for b in (0, 2, 4, 6, 8, 10, 12, 14)) + ", PD0"
        self.assertEqual(defs(m % (cols, 34))["GM_MATRIX_COL_RUNS"], 16)
        dec = MINIMAL_NUC + self.DECODER + (
            "key_matrix:\n  type: velocity\n  rows: { decoder: dec, first: 0, count: 4 }\n"
            "  columns: " + self.COLS % ("", "") + "\n  contacts: rows\n  keys: 4\n")
        self.assertEqual(defs(dec), {"GM_MATRIX_ROWS": 4, "GM_MATRIX_COL_RUNS": 1, "GM_MATRIX_DECODER": 1})
        self.assertNotIn("GM_MATRIX_ROWS", gd.generate(gd.load_device("x.yaml", MINIMAL_NUC)[0],
                                                        "x.yaml")["gm_device_config.h"])

    def test_image_stem(self):
        """Images are named <node name>-<version>, or <node name> without project.version."""
        dev, _ = gd.load_device("x.yaml", MINIMAL_NUC + "project: { version: 1.2.3 }\n")
        self.assertEqual(gb.image_stem(dev), "test-1.2.3")
        dev, _ = gd.load_device("x.yaml", MINIMAL_NUC)
        self.assertEqual(gb.image_stem(dev), "test")

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

    DECODER = ("decoder:\n  - id: dec\n    address: [PA2, PA3, PA4]\n"
               "    groups: [{ PA5: 1, PA6: 0 }, { PA5: 0, PA6: 1 }, { PA5: 0, PA6: 0 }]\n")
    COLS = ("[{ number: PB0, mode: INPUT_PULLUP, inverted: true%s }, "
            "{ number: PB1, mode: INPUT_PULLUP, inverted: true%s }]")

    def test_decoder_key_matrix(self):
        """Decoder rows (shorthand), contacts on row pairs and a key map, as on the Code 61."""
        km = ("key_matrix:\n  type: velocity\n  rows: { decoder: dec, first: 0, count: 4 }\n"
              "  columns: " + self.COLS % ("", "") + "\n  contacts: rows\n  keys: 4\n"
              "  key_map: [[3, 2], [1, 0]]\n")
        t = MINIMAL_NUC + self.DECODER + km
        self.assertClean(t)
        dev, _ = load(t)
        self.assertEqual([x.num for x in dev.matrix.rows], [0, 1, 2, 3])
        self.assertEqual(dev.matrix.key_map, [3, 2, 1, 0])
        dec = dev.hubs[0]
        self.assertEqual((dec.n_sel, dec.group_lv, dec.n_pins), (3, [1, 2, 0], 24))
        c = gd.generate(dev, "x.yaml")["gm_device_config.c"]
        self.assertIn(".type = GM_HUB_DECODER", c)
        self.assertIn(".n_groups = 3, .group_lv = { 1, 2, 0, 0 }", c)
        self.assertIn(".contacts = GM_MATRIX_CONTACTS_ROWS, .key_map = matrix_key_map", c)
        self.assertIn("static const uint8_t matrix_key_map[4] = {", c)
        # a key map with a hole, and keys beyond rows x columns are fine with a map
        t2 = t.replace("key_map: [[3, 2], [1, 0]]", "key_map: [[3, null], [1, 0]]")
        self.assertTrue(any("never play: 2" in w.msg for w in self.errs(t2, level="warning")))
        self.assertError(t.replace("[[3, 2], [1, 0]]", "[[3, 2]]"), r"one list per row pair: 2, got 1")
        self.assertError(t.replace("[[3, 2], [1, 0]]", "[[3, 2], [1]]"), r"needs 2 entries, one per column")
        self.assertError(t.replace("[[3, 2], [1, 0]]", "[[3, 3], [1, 0]]"), r"key 3 is already at")
        self.assertError(t.replace("[[3, 2], [1, 0]]", "[[4, 2], [1, 0]]"), r"key number 0\.\.3 or null")
        self.assertError(t.replace("count: 4", "count: 3").replace("key_map: [[3, 2], [1, 0]]\n", ""),
                         r"contacts: rows needs an even number of rows")
        self.assertError(t.replace("count: 4", "count: 30"), r"outputs 0\.\.29 are out of range")
        self.assertError(t.replace("type: velocity", "type: switch"), r"only for type: velocity")
        self.assertError(t.replace("contacts: rows", "contacts: diagonal"), r"'diagonal' is unknown")
        # explicit decoder pins work too
        t3 = t.replace("{ decoder: dec, first: 0, count: 4 }",
                       "[{ decoder: dec, number: 8 }, { decoder: dec, number: 9 }, { decoder: dec, number: 10 }, "
                       "{ decoder: dec, number: 11 }]")
        self.assertClean(t3)

    def test_decoder_errors(self):
        self.assertError(MINIMAL_NUC + "decoder:\n  - { id: d }\n", r"needs a list of 1-4 address pins")
        bad = self.DECODER.replace("{ PA5: 0, PA6: 1 }", "{ PA5: 0, PA7: 1 }")
        self.assertError(MINIMAL_NUC + bad, r"same enable pins")
        bad = self.DECODER.replace("{ PA5: 0, PA6: 1 }", "{ PA5: 1, PA6: 0 }")
        self.assertError(MINIMAL_NUC + bad, r"same enable levels as groups\[0\]")
        bad = self.DECODER.replace("{ PA5: 0, PA6: 1 }", "{ PA5: 2, PA6: 1 }")
        self.assertError(MINIMAL_NUC + bad, r"level must be 0 or 1")
        # a decoder output is a row select, not an LED
        self.assertError(MINIMAL_NUC + self.DECODER + "leds:\n  - { id: l, pin: { decoder: dec, number: 0 } }\n",
                         r"hub 'dec' is a decoder, which can't be an output")
        # no groups: one group of 2^n outputs
        dev, _ = load(MINIMAL_NUC + "decoder:\n  - { id: d, address: [PA2, PA3] }\n")
        self.assertEqual((dev.hubs[0].group_lv, dev.hubs[0].n_pins), ([0], 4))

    def test_switch_matrix(self):
        """Buttons and encoders on a switch matrix that shares the key matrix's columns."""
        sw = ("switch_matrix:\n  - id: panel\n    rows: { decoder: dec, first: 16, count: 2 }\n"
              "    columns: " + self.COLS % (", allow_other_uses: true", ", allow_other_uses: true") + "\n")
        km = ("key_matrix:\n  type: velocity\n  rows: { decoder: dec, count: 4 }\n"
              "  columns: " + self.COLS % (", allow_other_uses: true", ", allow_other_uses: true") +
              "\n  contacts: rows\n  keys: 4\n")
        btns = ("  - { id: up, pin: { switch_matrix: panel, number: 0 }, action: cc_momentary, cc: 20 }\n"
                "  - { id: down, pin: { switch_matrix: panel, number: 1 }, action: cc_momentary, cc: 21 }\n")
        enc = "encoders:\n  - { id: e, pin_a: { switch_matrix: panel, number: 2 }, " \
              "pin_b: { switch_matrix: panel, number: 3 }, cc: 30 }\n"
        t = MINIMAL_NUC + btns + sw + self.DECODER + km + enc   # the switch_matrix comes before its decoder
        self.assertClean(t)
        dev, _ = load(t)
        panel = dev.hubs[0]
        self.assertEqual(([x.num for x in panel.rows], len(panel.cols), panel.n_pins), ([16, 17], 2, 4))
        c = gd.generate(dev, "x.yaml")["gm_device_config.c"]
        self.assertIn("static const gm_pin_t hub1_rows[2] = {", c)
        self.assertIn(".rows = hub1_rows, .cols = hub1_cols, .n_rows = 2, .n_cols = 2,", c)
        self.assertIn(".type = GM_HUB_SWMATRIX", c)
        # shared columns need allow_other_uses on each use
        self.assertError(t.replace(", allow_other_uses: true", "", 1), r"PB0 is already used")
        # rows: only MCU pins or decoder outputs; size limit
        bad = t.replace("rows: { decoder: dec, first: 16, count: 2 }", "rows: [{ switch_matrix: panel, number: 0 }]")
        self.assertError(bad, r"which can't be an output")
        big = t.replace("first: 16, count: 2", "first: 0, count: 17")
        self.assertError(big, r"17 rows x 2 columns = 34 switches: at most 32")
        self.assertError(t.replace("number: 3 }, cc: 30", "number: 4 }, cc: 30"), r"pin 4 is out of range")
        # rows_per_scan: generated, range-checked, and a warning when it covers every row
        t2 = t.replace("    rows: { decoder: dec, first: 16, count: 2 }\n",
                       "    rows: { decoder: dec, first: 16, count: 2 }\n    rows_per_scan: 1\n")
        self.assertClean(t2)
        dev2, _ = load(t2)
        self.assertIn(".rows_per_scan = 1,", gd.generate(dev2, "x.yaml")["gm_device_config.c"])
        self.assertError(t2.replace("rows_per_scan: 1", "rows_per_scan: 0"), r"out of range \(1\.\.32\)")
        self.assertTrue(any("every row is read" in w.msg
                            for w in self.errs(t2.replace("rows_per_scan: 1", "rows_per_scan: 2"), level="warning")))

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
        t += "".join(f"  - {{ id: x{i}, pin: P{'C' if i < 16 else 'D'}{i % 16} }}\n" for i in range(33))
        self.assertError(t, r"65 buttons: at most 64")

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

    def test_safety_keys(self):
        """factory_reset_combo:, unlock:, status_led: (core/README.md "Factory-reset combo", "Physical unlock",
        "Status LED")."""
        two = MINIMAL_NUC + "  - { id: b, pin: PA2 }\n  - { id: c, pin: PA3 }\n"
        hdr = lambda text: gd.generate(load(text)[0])["gm_device_config.h"]   # noqa: E731
        # unlock: opt-in. Left out = off, even with a boot_combo; `true` takes the boot_combo
        h = hdr(two + "boot_combo: [a, b]\n")
        self.assertIn("#define GM_UNLOCK 0", h)
        self.assertNotIn("GM_UNLOCK_COMBO", h)
        h = hdr(two + "boot_combo: [a, b]\nunlock: true\n")
        self.assertIn("#define GM_UNLOCK 1", h)
        self.assertIn("#define GM_UNLOCK_COMBO 0x3u /* a + b */", h)
        self.assertIn("#define GM_UNLOCK_HOLD_MS 2000", h)
        self.assertIn("#define GM_UNLOCK_REPLY 9 ", h)            # 5 + (1 + 1) x 2
        self.assertIn("#define GM_RESET_COMBO 0", h)
        self.assertIn("#define GM_STATUS_LED (-1)", h)
        self.assertIn("#define GM_UNLOCK 0", hdr(two))
        self.assertIn("#define GM_UNLOCK 0", hdr(two + "boot_combo: [a, b]\nunlock: false\n"))
        h = hdr(two + "unlock: { combo: [c], hold: 0s }\n")
        self.assertIn("#define GM_UNLOCK_COMBO 0x4u /* c */", h)
        self.assertIn("#define GM_UNLOCK_HOLD_MS 0", h)
        self.assertIn("#define GM_UNLOCK_COMBO 0x3u", hdr(two + "boot_combo: [b, a]\nunlock: { hold: 3s }\n"))
        self.assertError(two + "unlock: true\n", r"unlock: true needs a combo")
        self.assertError(two + "unlock: { hold: 1s }\n", r"needs `combo:`")
        self.assertError(two + "unlock: { combo: [a], hold: 11s }\n", r"out of range")
        self.assertError(two + "unlock: { combo: [a], wait: 1s }\n", r"unknown key")
        self.assertError(two + "unlock: { combo: [] }\n", r"at least one button")
        self.assertError(two + "unlock: { combo: [a, zz] }\n", r"no button with id 'zz'", path="unlock.combo[1]")
        # the factory-reset combo, and its overlap with boot_combo
        h = hdr(two + "factory_reset_combo: [b, c]\n")
        self.assertIn("#define GM_RESET_COMBO 0x6u /* b + c */", h)
        self.assertIn("#define GM_RESET_HOLD_MS 5000", h)
        self.assertIn("#define GM_RESET_HOLD_MS 8000", hdr(two + "factory_reset_combo: { combo: [c], hold: 8s }\n"))
        self.assertClean(two + "boot_combo: [a, b]\nfactory_reset_combo: [b, c]\n")
        for combo, how in (("[a, b]", "the same buttons as"), ("[a]", "inside"), ("[a, b, c]", "a superset of")):
            self.assertError(two + f"boot_combo: [a, b]\nfactory_reset_combo: {combo}\n", how,
                             path="factory_reset_combo")
        self.assertError(two + "factory_reset_combo: { combo: [a], hold: 0s }\n", r"out of range")
        self.assertError(two + "factory_reset_combo: { hold: 5s }\n", r"needs `combo:`")
        # status_led: an LED id
        led = two + "leds: [{ id: st, pin: PA4 }]\n"
        self.assertIn("#define GM_STATUS_LED 0 /* st */", hdr(led + "status_led: st\n"))
        self.assertError(led + "status_led: a\n", r"'a' is the id of buttons\[0\] \(a button\), not of an LED")

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
FEEDBACK_TEST = os.path.join(REPO, "core", "test", "feedback-test.yaml")

FB_NUC = MINIMAL_NUC + """\
leds:
  - { id: l1, pin: PC0 }
  - { id: l2, pin: PC1 }
"""


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestFeedback(unittest.TestCase):
    """Host feedback (F1), LED ownership, the USB DAW port and Mackie Control (core/README.md
    "DAW feedback")."""
    errs = TestValidation.errs
    assertError = TestValidation.assertError

    def header(self, text):
        dev, diags = load(text)
        self.assertIsNotNone(dev, [str(d) for d in diags])
        files = gd.generate(dev, "t.yaml")
        return files["gm_device_config.h"], files["gm_device_config.c"]

    def warns(self, text):
        return [str(d) for d in self.errs(text, level="warning")]

    def test_keystation_defaults(self):
        dev, _ = gd.load_device(KEYSTATION)
        h = gd.generate(dev, "k.yaml")["gm_device_config.h"]
        for d in ("GM_FEEDBACK 1", "GM_LED_FB 0", "GM_LED_OWNER 0", "GM_LED_TIMEOUT 200 /* 2s",
                  "GM_ANALOG_PICKUP 0", "GM_MCU 0", "GM_USB_DAW_PORT 0"):
            self.assertIn(f"#define {d}", h)
        self.assertNotIn("GM_DAW_PORT_NAME", h)

    def test_feedback_section(self):
        h, _ = self.header(FB_NUC + "feedback: { enabled: false }\n")
        self.assertIn("#define GM_FEEDBACK 0", h)
        h, _ = self.header(FB_NUC + "feedback: { led_owner: local, led_timeout: never }\n")
        self.assertIn("#define GM_LED_OWNER 2 /* local */", h)
        self.assertIn("#define GM_LED_TIMEOUT 65535 /* never", h)
        h, _ = self.header(FB_NUC + "feedback: { led_timeout: 250ms }\n")
        self.assertIn("#define GM_LED_TIMEOUT 25 ", h)
        self.assertError(FB_NUC + "feedback: { led_owner: daw }\n", r"unknown owner 'daw'.*host_then_local")
        self.assertError(FB_NUC + "feedback: { led_timeout: 2 }\n", r"needs a unit")
        self.assertError(FB_NUC + "feedback: { enable: false }\n", r"did you mean 'enabled'")

    def test_led_sources(self):
        h, c = self.header(FB_NUC.replace("{ id: l1, pin: PC0 }",
                                          "{ id: l1, pin: PC0, feedback: { note: 40, channel: 16 } }")
                           .replace("{ id: l2, pin: PC1 }",
                                    "{ id: l2, pin: PC1, feedback: { clock: beat }, owner: host }"))
        self.assertIn("#define GM_LED_FB 1", h)
        self.assertIn("{ GM_FB_NOTE, 40, 16, GM_OWNER_HOST_THEN_LOCAL, 200 }, /* l1 */", c)
        self.assertIn("{ GM_FB_CLOCK, 0, 0, GM_OWNER_HOST, 200 }, /* l2 */", c)
        h, c = self.header(FB_NUC.replace("{ id: l1, pin: PC0 }",
                                          "{ id: l1, pin: PC0, feedback: { transport: playing }, timeout: never }"))
        self.assertIn("{ GM_FB_PLAYING, 0, 0, GM_OWNER_HOST_THEN_LOCAL, GM_TIMEOUT_NEVER }", c)
        bad = FB_NUC.replace("{ id: l1, pin: PC0 }", "{ id: l1, pin: PC0, feedback: %s }")
        self.assertError(bad % "{ note: 1, cc: 2 }", r"only one of note, cc")
        self.assertError(bad % "{ channel: 2 }", r"give one of note: / cc:")
        self.assertError(bad % "{ clock: bar }", r"'bar' isn't a clock source \(valid: beat\)")
        self.assertError(bad % "{ clock: beat, channel: 2 }", r"only for note: / cc:")
        self.assertError(bad % "{ cc: 200 }", r"out of range")
        self.assertError(bad % "{ mcu: play }", r"needs an `mcu:` section")
        self.assertError(bad % "{ cc: 1 }" + "feedback: { enabled: false }\n", r"feedback is off")
        self.assertError(FB_NUC.replace("{ id: l1, pin: PC0 }", "{ id: l1, pin: PC0, owner: daw }"),
                         r"unknown owner 'daw'")
        w = self.warns(FB_NUC.replace("{ id: l1, pin: PC0 }", "{ id: l1, pin: PC0, owner: host, timeout: 1s }"))
        self.assertTrue(any("only used with owner: host_then_local" in x for x in w), w)

    def test_takeover(self):
        knob = MINIMAL_NUC + "analog:\n  - { id: k, pin: PA0, cc: 7%s }\n"
        h, c = self.header(knob % ", takeover: pickup")
        self.assertIn("#define GM_ANALOG_PICKUP 1", h)
        self.assertIn("gm_analog_pickup[GM_N_ANALOGS] = { 1 };", c)
        h, _ = self.header(knob % ", takeover: none")
        self.assertIn("#define GM_ANALOG_PICKUP 0", h)
        self.assertError(knob % ", takeover: scale", r"unknown takeover 'scale'")
        self.assertError(knob % ", takeover: pickup" + "feedback: { enabled: false }\n", r"pickup needs the host")

    def test_daw_port_and_mcu(self):
        h, c = self.header(MINIMAL_NUC + "mcu:\n")
        self.assertIn("#define GM_MCU 1", h)
        self.assertIn("#define GM_USB_DAW_PORT 1", h)
        self.assertIn("#define GM_MCU_CABLE 1", h)
        self.assertIn("#define GM_MCU_ID 0x14", h)
        self.assertIn('#define GM_DAW_PORT_NAME "test DAW"', h)
        self.assertIn("gm_encoder_ring[1] = { { 0, 0, GM_RING_NO_CENTER } };", c)
        h, _ = self.header(MINIMAL_NUC + "mcu: { extender: true }\n")
        self.assertIn("#define GM_MCU_ID 0x15", h)
        h, _ = self.header(MINIMAL_NUC.replace("pid: 0x0002 }", "pid: 0x0002, daw_port: false }") + "mcu: {}\n")
        self.assertIn("#define GM_USB_DAW_PORT 0", h)
        self.assertIn("#define GM_MCU_CABLE 0", h)
        self.assertIn("#define GM_FEEDBACK 0", h)      # the only port is the MCU port
        self.assertError(MINIMAL_NUC.replace("pid: 0x0002 }", "pid: 0x0002, daw_port: true }"),
                         r"DAW port carries Mackie Control: add an `mcu:`")
        esp = MINIMAL_ESP.format(plat="esp32", tr="serial")
        self.assertError(esp + "usb: { vid: 1, pid: 2, daw_port: true }\nmcu: {}\n", r"needs USB MIDI")
        w = self.warns(esp + "mcu: {}\nbuttons: [{ id: n, pin: GPIO4, action: note, note: 60 }]\n")
        self.assertTrue(any("Mackie Control on the only port" in x for x in w), w)
        self.assertError(esp + "mcu: {}\nleds: [{ id: l, pin: GPIO4, feedback: { cc: 3 } }]\n",
                         r"read as Mackie Control")

    def test_mcu_components(self):
        base = FB_NUC + "mcu: {}\n"
        h, c = self.header(base.replace("action: cc_momentary, cc: 1", "mcu: play, led: l1") +
                           "analog: [{ id: f, pin: PA0, mcu: master }]\n"
                           "encoders: [{ id: e, pin_a: PB0, pin_b: PB1, mcu: vpot_3, ring: [l2] }]\n")
        self.assertIn("gm_button_action_defaults[GM_N_BUTTONS] = { 16 }; /* mcu */", c)
        self.assertIn("gm_button_number_defaults[GM_N_BUTTONS] = { 94 };", c)
        self.assertIn("{ GM_FB_NONE, 0, 0, GM_OWNER_HOST, 200 }, /* l1 */", c)   # MCU-driven: host
        self.assertIn("{ GM_FB_NONE, 0, 0, GM_OWNER_HOST, 200 }, /* l2 */", c)   # ring LED
        self.assertIn("gm_analog_mcu[GM_N_ANALOGS] = { 8 };", c)
        self.assertIn("gm_analog_pickup[GM_N_ANALOGS] = { 1 };", c)                 # MCU fader default
        self.assertIn("gm_encoder_mcu[GM_N_ENCODERS] = { 2 };", c)
        self.assertIn("static const uint8_t ring_0[1] = { 1 };", c)
        self.assertIn("{ ring_0, 1, GM_RING_NO_CENTER }", c)
        self.assertIn("#define GM_LED_FB 1", h)
        _, c = self.header(base + "encoders: [{ id: e, pin_a: PB0, pin_b: PB1, mcu: vpot_1, ring_center: l1 }]\n")
        self.assertIn("{ 0, 0, 0 }", c)                                            # centre LED only
        self.assertError(base + "feedback: { script: false }\n"
                         "encoders: [{ id: e, pin_a: PB0, pin_b: PB1, cc: 3, ring_center: l1 }]\n",
                         r"ring_center: is driven by Mackie Control or a DAW script")
        btn = MINIMAL_NUC.replace("action: cc_momentary, cc: 1", "%s")
        self.assertError(btn % "mcu: play", r"needs an `mcu:` section")
        self.assertError(btn % "action: mcu" + "mcu: {}\n", r"action mcu needs mcu:")
        self.assertError(btn % "mcu: plya" + "mcu: {}\n", r"unknown MCU button 'plya'; did you mean 'play'")
        self.assertError(btn % "action: note, note: 3, mcu: play" + "mcu: {}\n", r"mcu: is for action: mcu")
        self.assertError(btn % "mcu: play, cc: 3" + "mcu: {}\n", r"takes mcu:.*not cc:")
        self.assertError(base + "analog: [{ id: f, pin: PA0, mcu: fader_9 }]\n", r"unknown MCU control 'fader_9'")
        self.assertError(base + "analog: [{ id: f, pin: PA0 }]\n", r"is required", path="analog[0].cc")
        self.assertError(base + "analog: [{ id: f, pin: PA0, mcu: fader_1 }, { id: g, pin: PA1, mcu: fader_1 }]\n",
                         r"'fader_1' is already used by analog\[0\]")
        self.assertError(base + "feedback: { script: false }\n"
                         "encoders: [{ id: e, pin_a: PB0, pin_b: PB1, cc: 3, ring: [l1] }]\n",
                         r"give the encoder `mcu: vpot_N`")
        self.assertError(base + "encoders: [{ id: e, pin_a: PB0, pin_b: PB1, mcu: vpot_1, ring: [nope] }]\n",
                         r"no LED with id 'nope'")

    def test_local_led_without_button(self):
        w = self.warns(FB_NUC.replace("{ id: l2, pin: PC1 }", "{ id: l2, pin: PC1, owner: local }"))
        self.assertTrue(any("never lights" in x for x in w), w)
        w = self.warns(FB_NUC.replace("{ id: l1, pin: PC0 }", "{ id: l1, pin: PC0, owner: local }")
                       .replace("action: cc_momentary, cc: 1", "action: cc_momentary, cc: 1, led: l1"))
        self.assertFalse(any("never lights" in x for x in w), w)

    def test_daw_roles_and_components(self):
        base = FB_NUC
        h, c = self.header(base.replace("action: cc_momentary, cc: 1", "action: cc_momentary, cc: 1, daw: play"))
        self.assertIn("#define GM_HOST_FB 1", h)
        self.assertIn("#define GM_RINGS 1", h)
        self.assertIn('const gm_comp_t gm_comp_buttons[GM_N_BUTTONS] = { { "a", "A", "play" } };', c)
        self.assertIn('gm_comp_matrix[1] = { { "", "", "" } };', c)
        self.assertIn('gm_comp_displays[1] = { { "", "", "" } };', c)          # no displays
        # displays (GET_COMPONENT kind 6): the Code 61's one, named after its id
        path = os.path.join(REPO, "devices", "m-audio-code61.yaml")
        with open(path, encoding="utf-8") as f:
            d, diags = gd.load_device(path, f.read())
        self.assertIsNotNone(d, [str(x) for x in diags if x.level == "error"])
        c61 = gd.generate(d, "m-audio-code61.yaml")["gm_device_config.c"]
        self.assertIn('const gm_comp_t gm_comp_displays[GM_N_DISPLAYS] = { { "display", "Display", "param" } };', c61)
        h, c = self.header(base + "feedback: { script: false }\n")
        self.assertIn("#define GM_HOST_FB 0", h)
        self.assertNotIn("gm_comp_buttons", c)
        h, _ = self.header(base + "feedback: { enabled: false }\n")
        self.assertIn("#define GM_HOST_FB 0", h)
        btn = base.replace("action: cc_momentary, cc: 1", "action: cc_momentary, cc: 1, daw: %s")
        self.assertError(btn % "plya", r"unknown DAW role 'plya' \(did you mean 'play'")
        self.assertError(btn % "volume_9", r"unknown DAW role")
        self.header(btn % "x_my_thing")                                           # free x_ names
        for role in ("nav_back", "nav_scroll", "clip_8_8", "scene_1", "grid_left", "knob_3", "assign_pan", "flip",
                     "view_buses", "shift", "alt", "strip_next", "send_a_1", "send_b_8", "master_solo",
                     "master_select", "marker_add", "nudge_back", "zoom_in", "scrub", "automation_latch", "save",
                     "time_format", "time", "track_name", "param", "status", "meter_8", "master_meter"):
            self.header(btn % role)
        for role in ("clip_9_1", "clip_1_0", "scene_9", "send_c_1", "meter_9"):
            self.assertError(btn % role, r"unknown DAW role")
        # <host>.<command>: one host's own, lower case
        self.header(btn % "zynthian.toggle_audio_play")
        self.header(btn % "live.x2")
        for role in ("Zynthian.play", "zynthian.", ".play", "zyn-thian.play", "2yn.play", "zynthian.play.x"):
            self.assertError(btn % f'"{role}"', r"unknown DAW role")
        self.assertError(btn % ("zynthian." + "a" * 30), r"at most 31")
        w = self.warns(btn % "play" + "feedback: { script: false }\n")
        self.assertTrue(any("feedback.script is off" in x for x in w), w)
        # a ring without Mackie Control, for a DAW script
        _, c = self.header(base + "encoders: [{ id: e, pin_a: PB0, pin_b: PB1, cc: 3, daw: device_param_1, "
                                  "ring: [l1], ring_center: l2 }]\n")
        self.assertIn("{ ring_0, 1, 1 }", c)
        self.assertIn('{ "e", "E", "device_param_1" }', c)
        self.assertError(base + "feedback: { script: false }\n"
                         "encoders: [{ id: e, pin_a: PB0, pin_b: PB1, cc: 3, ring: [l1] }]\n",
                         r"leave feedback.script on")

    def test_touch_and_mcu_roles(self):
        base = FB_NUC + "analog:\n  - { id: f, pin: PA0, cc: 7, touch: %s }\n"
        h, c = self.header(base % "movement")
        self.assertIn("#define GM_ANALOG_TOUCH 1", h)
        self.assertIn("const uint8_t gm_analog_touch[GM_N_ANALOGS] = { 2 };", c)
        h, c = self.header(base % "none")
        self.assertIn("#define GM_ANALOG_TOUCH 0", h)
        self.assertNotIn("gm_analog_touch", c)
        self.assertError(base % "{ pin: PA2 }", r"a touch sensor isn't supported yet")
        self.assertError(base % "moving", r"unknown touch 'moving' \(valid: none, movement\)")
        # mcu: channel_left / channel_right without daw: are strip_prev / strip_next; others as they are
        self.assertEqual(gd.comp_role(gd.Hub(daw=None), "channel_left"), "strip_prev")
        self.assertEqual(gd.comp_role(gd.Hub(daw=None), "channel_right"), "strip_next")
        self.assertEqual(gd.comp_role(gd.Hub(daw=None), "bank_left"), "bank_left")
        self.assertEqual(gd.comp_role(gd.Hub(daw="track_prev"), "channel_left"), "track_prev")
        self.assertEqual(gd.comp_role(gd.Hub(daw=None)), "")

    def test_feedback_test_device(self):
        """core/test's feedback device validates cleanly (test_feedback.c runs on it)."""
        dev, diags = gd.load_device(FEEDBACK_TEST)
        self.assertIsNotNone(dev, [str(d) for d in diags])
        self.assertEqual([str(d) for d in diags if d.level != "info"], [])


@unittest.skipUnless(GCC and gd.yaml, "needs host gcc and PyYAML")
class TestFeedbackCompiles(unittest.TestCase):
    setUp = TestGenerateCompiles.setUp
    tearDown = TestGenerateCompiles.tearDown
    gcc = TestGenerateCompiles.gcc
    compile_with_core = TestGenerateCompiles.compile_with_core

    def test_feedback_device_compiles(self):
        dev, _ = gd.load_device(FEEDBACK_TEST)
        self.compile_with_core(gen_dir(dev, self.tmp, "fb"))


# ---- presets (research/core/presets-design.md §2, §10) ----

PRESET_NUC = MINIMAL_NUC + """\
  - { id: b, pin: PA2, action: note, note: 60 }
  - { id: c, pin: PA3, action: octave_up }
analog:
  - { id: knob, pin: PA0, cc: 7 }
encoders:
  - { id: enc, pin_a: PB0, pin_b: PB1, cc: 20 }
key_matrix:
  type: velocity
  rows: [PA8, PA9]
  columns: [PB2, PB3]
  keys: 2
"""


def presets(body, base=PRESET_NUC):
    """base + a presets: block (body: the lines under it, indented by 2)."""
    return base + "presets:\n" + "".join(f"  {ln}\n" for ln in body.strip("\n").split("\n"))


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestPresets(unittest.TestCase):
    errs = TestValidation.errs
    assertError = TestValidation.assertError
    assertClean = TestValidation.assertClean

    def gen(self, text):
        dev, diags = load(text)
        self.assertIsNotNone(dev, [str(d) for d in diags])
        files = gd.generate(dev, "t.yaml")
        return dev, files["gm_device_config.h"], files["gm_device_config.c"]

    def warns(self, text):
        return [d.msg for d in self.errs(text, level="warning")]

    def test_no_presets(self):
        """A device without presets: one slot, "Default", no overrides; otherwise as before."""
        dev, h, c = self.gen(MINIMAL_NUC)
        self.assertEqual(dev.n_presets, 1)
        self.assertIn("#define GM_N_PRESETS 1 /* no presets: in the YAML */", h)
        self.assertIn("#define GM_PRESETS_REVISION 0", h)
        self.assertIn('    { "Default", GM_PROGRAM_NONE, 0, 0 }, /* slot 1 */\n};', c)
        self.assertNotIn("gm_preset_val_t", c)
        self.assertNotIn("presets:", gb.summary(dev))

    def test_generate(self):
        dev, h, c = self.gen(presets("""
slots: 5
revision: 3
list:
  - { name: Main, program: 0 }
  - name: Drums
    program: 9
    midi: { channel: 10, transpose: -2, octave: 1 }
    velocity: { curve: hard, fixed: 90 }
    bend: { glide: 0ms }
    ramp: { time: 1s }
    buttons:
      a: { action: note, note: 36 }
      b: { note: 38 }
      c: { action: preset_select, preset: 5 }
    analog: { knob: { cc: 11 } }
    encoders: { enc: { cc: 21, mode: relative_offset } }
  - name: Empty copy
"""))
        self.assertIn("#define GM_N_PRESETS 5 /* presets.slots */", h)
        self.assertIn("#define GM_PRESETS_REVISION 3", h)
        vals = re.findall(r"\{ (\d+), (\d+), (-?\d+) \}, /\* (.*?) \*/", c.split("presets: overrides")[1])
        self.assertEqual([(int(a), int(b), int(x)) for a, b, x, _ in vals], [
            (1, 0, 10), (2, 0, -2), (3, 0, 1), (4, 0, 2), (5, 0, 90), (8, 0, 11), (9, 0, 4), (9, 2, 15),
            (10, 0, 36), (10, 1, 38), (10, 2, 5), (11, 0, 0), (12, 0, 1000), (15, 0, 21), (16, 0, 2)])
        self.assertIn("velocity.curve = hard", c)
        self.assertIn("button.action[c] = preset_select", c)
        self.assertIn("static const gm_preset_val_t preset_2_vals[15] = { /* Drums */", c)
        self.assertIn('{ "Main", 0, 0, 0 }, /* slot 1 */', c)
        self.assertIn('{ "Drums", 9, 15, preset_2_vals }, /* slot 2 */', c)
        self.assertIn('{ "Empty copy", GM_PROGRAM_NONE, 0, 0 }, /* slot 3 */', c)
        self.assertIn('{ "Preset 4", GM_PROGRAM_NONE, 0, 0 }, /* slot 4 */', c)
        self.assertIn('{ "Preset 5", GM_PROGRAM_NONE, 0, 0 }, /* slot 5 */', c)
        self.assertIn("presets: 5 slots, worst case", gb.summary(dev))

    def test_default_slots_and_names(self):
        dev, h, c = self.gen(presets("list:\n  - {}\n  - { midi: { channel: 2 } }"))
        self.assertIn("#define GM_N_PRESETS 2", h)
        self.assertIn('{ "Default", GM_PROGRAM_NONE, 0, 0 }', c)
        self.assertIn('{ "Preset 2", GM_PROGRAM_NONE, 1, preset_2_vals }', c)
        dev, h, c = self.gen(presets("slots: 3"))
        self.assertEqual([p.name for p in dev.presets], ["Default", "Preset 2", "Preset 3"])

    def test_same_as_default_dropped(self):
        t = presets("list:\n  - {}\n  - midi: { channel: 1 }\n    buttons: { a: { cc: 1 }, b: { action: note } }\n"
                    "    analog: { knob: { cc: 7 } }\n    encoders: { enc: { mode: absolute } }")
        ws = self.warns(t)
        self.assertEqual(sum("the same as in preset 1" in w for w in ws), 5, ws)
        self.assertTrue(any("midi.channel = 1" in w for w in ws), ws)
        dev, _h, c = self.gen(t)
        self.assertEqual(dev.presets[1].vals, [])
        self.assertNotIn("gm_preset_val_t", c)

    def test_action_numbers(self):
        """preset_next 13, preset_prev 14, preset_select 15 (number = slot), mcu 16."""
        btns = MINIMAL_NUC.replace("  - { id: a, pin: PA1, action: cc_momentary, cc: 1 }\n",
                                   "  - { id: a, pin: PA1, action: preset_next }\n"
                                   "  - { id: b, pin: PA2, action: preset_prev }\n"
                                   "  - { id: c, pin: PA3, action: preset_select, preset: 4 }\n"
                                   "  - { id: d, pin: PA4, action: program_prev }\n"
                                   "  - { id: e, pin: PA5, mcu: play }\nmcu: {}\n")
        _dev, _h, c = self.gen(btns + "presets: { slots: 4 }\n")
        self.assertIn("gm_button_action_defaults[GM_N_BUTTONS] = { 13, 14, 15, 12, 16 };", c)
        self.assertIn("gm_button_number_defaults[GM_N_BUTTONS] = { 0, 0, 4, 0, 94 };", c)
        self.assertEqual(gd.ACTION_IDS["mcu"], 16)
        self.assertEqual(gd.ACTIONS.index("preset_select"), 15)

    def test_preset_action_errors(self):
        btn = MINIMAL_NUC.replace("action: cc_momentary, cc: 1", "%s")
        self.assertError(btn % "action: preset_next", r"this device has 1 preset slot: add presets:")
        self.assertError(btn % "action: preset_select, preset: 2" + "presets: { slots: 1 }\n",
                         r"switches presets.*1 preset slot")
        withp = btn + "presets: { slots: 3 }\n"
        self.assertClean(withp % "action: preset_select, preset: 3")
        self.assertError(withp % "action: preset_select, preset: 4", r"4 is out of range \(1\.\.3\)")
        self.assertError(withp % "action: preset_select", r"action preset_select needs preset: \(1\.\.3")
        self.assertError(withp % "action: preset_select, cc: 3", r"takes preset:, not cc:")
        self.assertError(withp % "action: preset_next, preset: 2", r"action preset_next takes no a preset number")
        self.assertError(withp % "action: note, preset: 2", r"action note takes note:, not preset:")
        self.assertError(withp % "action: preset_nxt", r"did you mean 'preset_next'")

    def test_section_errors(self):
        self.assertError(MINIMAL_NUC + "presets: [a]\n", r"must be a mapping", path="presets")
        self.assertError(presets("slots: 8\nlists: []"), r"unknown key \(did you mean 'list'\?\)", path="presets.lists")
        self.assertEqual(len(self.errs(presets("slots: 8\nlists: []"))), 1)     # reported once
        self.assertError(presets("slots: 0"), r"0 is out of range \(1\.\.64\)")
        self.assertError(presets("slots: 65"), r"65 is out of range \(1\.\.64\)")
        self.assertError(presets("slots: 1\nlist: [{ name: A }, { name: B }]"),
                         r"1 slot can't hold the 2 presets in presets.list")
        self.assertError(presets("revision: 16384"), r"16384 is out of range \(0\.\.16383\)")
        self.assertClean(presets("revision: 16383"))
        self.assertError(presets("list: { name: A }"), r"must be a list", path="presets.list")

    def test_names(self):
        self.assertError(presets("list: [{ name: A }, { name: a }]"), r"'a' is already the name of presets.list\[0\]")
        self.assertError(presets("slots: 3\nlist: [{ name: A }, { name: Preset 3 }]"),
                         r"'Preset 3' is the name of empty slot 3")
        self.assertError(presets("list: [{ name: 12345678901234567 }]"), r"17 characters: at most 16")
        self.assertClean(presets("list: [{ name: 1234567890123456 }, { name: 808 }]"))
        self.assertError(presets("list: [{ name: Café }]"), r"printable ASCII")
        self.assertError(presets("list: [{ name: '  ' }]"), r"non-empty")
        dev, _ = load(presets("list: [{ name: 808 }]"))
        self.assertEqual(dev.presets[0].name, "808")

    def test_preset_1_only_name_and_program(self):
        self.assertError(presets("list:\n  - { name: A, midi: { channel: 2 } }"),
                         r"preset 1 is the component defaults", path="presets.list[0].midi")
        self.assertError(presets("list:\n  - { name: A, program: 128 }"), r"128 is out of range \(0\.\.127\)")
        self.assertError(presets("list:\n  - {}\n  - { leds: {} }"), r"unknown key", path="presets.list[1].leds")

    def test_override_errors(self):
        two = "list:\n  - {}\n  - "
        self.assertError(presets(two + "buttons: { aa: { cc: 2 } }"), r"no button with id 'aa'.*did you mean 'a'")
        self.assertError(presets(two + "buttons: { knob: { cc: 2 } }"), r"'knob' is the id of analog\[0\] "
                                                                         r"\(an analog input\), not of a button")
        self.assertError(presets(two + "analog: { a: { cc: 2 } }"), r"not of an analog input")
        self.assertError(presets(two + "buttons: { a: { pin: PA5 } }"), r"pin: is the same in every preset",
                         path="presets.list[1].buttons.a.pin")
        self.assertError(presets(two + "analog: { knob: { adc_min: 3 } }"), r"adc_min: is the same in every")
        self.assertError(presets(two + "buttons: { a: { cc: 200 } }"), r"200 is out of range \(0\.\.127\)")
        self.assertError(presets(two + "buttons: { a: { note: 2 } }"), r"action cc_momentary takes cc:, not note:")
        self.assertError(presets(two + "buttons: { c: { cc: 2 } }"), r"action octave_up takes no a CC or note")
        self.assertError(presets(two + "buttons: { a: { action: note } }"), r"action note needs note:")
        self.assertError(presets(two + "buttons: { a: { action: preset_select, preset: 3 } }"),
                         r"3 is out of range \(1\.\.2\)")
        self.assertError(presets(two + "buttons: { a: { mcu: play } }"), r"mcu: is for action: mcu")
        self.assertError(presets(two + "buttons: [a]"), r"must be a mapping of button ids")
        self.assertError(presets(two + "encoders: { enc: { mode: relative_ofset } }"),
                         r"unknown mode 'relative_ofset' \(did you mean 'relative_offset'")
        self.assertError(presets(two + "encoders: { enc: { steps_per_detent: 2 } }"), r"same in every preset")
        self.assertTrue(any("changes nothing" in w for w in self.warns(presets(two + "buttons: { a: {} }"))))

    def test_global_errors(self):
        two = "list:\n  - {}\n  - "
        self.assertError(presets(two + "midi: { channel: 17 }"), r"17 is out of range \(1\.\.16\)")
        self.assertError(presets(two + "midi: { transpose: 25 }"), r"25 is out of range \(-24\.\.24\)")
        self.assertError(presets(two + "midi: { octave: -5 }"), r"-5 is out of range \(-4\.\.4\)")
        self.assertError(presets(two + "midi: { chanel: 2 }"), r"did you mean 'channel'")
        self.assertError(presets(two + "midi: { transport: usb }"), r"the MIDI transport is the device's")
        self.assertError(presets(two + "velocity: { fast: 1ms }"), r"global .*key_matrix.velocity")
        self.assertError(presets(two + "velocity: { curve: hrad }"), r"unknown curve 'hrad' \(did you mean 'hard'")
        self.assertError(presets(two + "velocity: { fixed: 0 }"), r"0 is out of range \(1\.\.127\)")
        self.assertError(presets(two + "bend: { glide: 50 }"), r"needs a unit")
        self.assertError(presets(two + "bend: { glide: 3s }"), r"3s is out of range \(0us\.\.2000ms\)")
        self.assertError(presets(two + "ramp: { time: 6s }"), r"out of range")
        self.assertError(presets(two + "velocity: { curve: hard }", base=MINIMAL_NUC),
                         r"velocity.curve doesn't exist on this device: it needs a velocity key matrix")
        nobtn = MINIMAL_ESP.format(plat="esp32", tr="serial") + "analog: [{ id: k, pin: GPIO34, cc: 1 }]\n"
        self.assertError(presets(two + "bend: { glide: 10ms }", base=nobtn), r"bend.glide doesn't exist.*no buttons")
        self.assertClean(presets(two + "velocity: { fixed: 100 }"))     # same as default: a warning only

    def test_analog_mode_preset(self):
        dev, diags = load(presets("""
slots: 2
list:
  - { name: Main }
  - { name: Bend, analog: { knob: { mode: pitch_bend } } }
"""))
        self.assertIsNotNone(dev, [str(d) for d in diags])
        self.assertIn((29, 0, 1, "analog.mode[knob] = pitch_bend"), dev.presets[1].vals)
        self.assertError(presets("""
slots: 2
list:
  - { name: Main }
  - { name: Bend, analog: { knob: { mode: wobble } } }
"""), r"pitch_bend")

    def test_budget_nuc(self):
        """NUC: 6 usable pages of 512 B (one of 7 spare); records never span a page."""
        dev, diags = load(presets("slots: 4"))
        bd = dev.budget
        # per-preset: channel, transpose, octave, curve, fixed, analog cc + mode, buttons 3 x 2, glide,
        # ramp, encoder 2, program = 18 values; globals: fast, slow, events, log_level, active, count = 6
        self.assertEqual((bd["preset_values"], bd["global_values"]), (18, 6))
        self.assertEqual((bd["preset_record"], bd["global_record"]), (4 * (9 + 36), 4 * (9 + 12)))
        self.assertEqual((bd["used"], bd["capacity"], bd["unit"]), (2, 5, "pages"))   # 2 per page; globals fit
        self.assertEqual(bd["max_slots"], 10)                                          # 5 pages x 2
        self.assertEqual([d for d in diags if "presets use" in d.msg], [])
        self.assertTrue(any("presets use 100% of the nuvoton-nuc1xx data-flash journal" in w and
                            "Up to 10 slots fit; 8 or fewer stay under 85%" in w
                            for w in self.warns(presets("slots: 10"))), self.warns(presets("slots: 10")))
        e = self.assertError(presets("slots: 11"), r"presets don't fit the nuvoton-nuc1xx data-flash journal",
                             path="presets.slots")
        self.assertIn("At most 10 slots fit (slots: 10)", e.msg)
        self.assertIn("6 of the 5 usable 512 B pages", e.msg)
        # one record bigger than a page: 32 buttons = 64 values + 7 others
        pins = [f"P{p}{n}" for p in "AB" for n in range(16)]
        many = MINIMAL_NUC.replace("  - { id: a, pin: PA1, action: cc_momentary, cc: 1 }\n",
                                   "".join(f"  - {{ id: b{i}, pin: {pn} }}\n" for i, pn in enumerate(pins)))
        self.assertTrue(any(re.search(r"the settings don't fit one record of the nuvoton-nuc1xx data-flash "
                                      r"journal \(at most 512 B\)", w) for w in self.warns(many)))
        self.assertClean(many)        # without presets: a warning, as with the v1 record
        self.assertIn("71 per-preset values", self.assertError(many + "presets: { slots: 2 }\n", r"one record").msg)

    def test_budget_esp8266(self):
        """ESP8266: every slot in one 4096 B EEPROM image."""
        base = MINIMAL_ESP.format(plat="esp8266", tr="serial") + "buttons: [{ id: a, pin: GPIO4, action: cc_toggle, cc: 3 }]\n"
        dev, _ = load(presets("slots: 2", base=base))
        bd = dev.budget
        self.assertEqual((bd["preset_record"], bd["global_record"], bd["capacity"], bd["unit"]), (108, 68, 4096, "B"))
        self.assertEqual(bd["max_slots"], 37)          # (4096 - 68) // 108
        self.assertEqual(bd["max_slots_calm"], 31)     # 31 * 108 + 68 = 3416 <= 0.85 * 4096
        self.assertFalse(any("presets use" in w for w in self.warns(presets("slots: 31", base=base))))
        self.assertTrue(any("presets use 86% of the esp8266 EEPROM sector" in w
                            for w in self.warns(presets("slots: 32", base=base))))
        e = self.assertError(presets("slots: 38", base=base), r"don't fit the esp8266 EEPROM sector")
        self.assertIn("4172 of 4096 B", e.msg)
        self.assertIn("At most 37 slots", e.msg)

    def test_budget_esp32(self):
        """ESP32: one NVS blob per slot (<= 1984 B), 12 KB for all."""
        gp = [13, 14, 16, 17, 18, 19, 21]
        base = MINIMAL_ESP.format(plat="esp32", tr="serial") + "buttons:\n" + "".join(
            f"  - {{ id: b{g}, pin: GPIO{g} }}\n" for g in gp)
        dev, _ = load(presets("slots: 30", base=base))
        bd = dev.budget
        self.assertEqual((bd["preset_values"], bd["preset_record"]), (21, 204))
        self.assertEqual(bd["max_slots"], 59)          # (12288 - 60) // 204
        self.assertFalse(any("presets use" in w for w in self.warns(presets("slots: 50", base=base))))
        self.assertTrue(any("presets use" in w and "NVS" in w for w in self.warns(presets("slots: 51", base=base))))
        self.assertError(presets("slots: 64", base=base), r"don't fit the esp32 NVS.*At most 59 slots")
        for plat in ("esp32s2", "esp32s3"):
            self.assertEqual(gd.STORE_BUDGETS[plat], gd.STORE_BUDGETS["esp32"])

    def test_budget_stm32f1(self):
        """STM32F1: packed (v3) records in a log over 3 pages of 1 KB, one kept spare: every slot's record
        together (L) and the largest (M) need L + M <= 2048 B. The Code 61's worst case is the one
        platform/stm32f1/test/cs-test writes: 67 group words a preset, 18 for the globals."""
        path = os.path.join(REPO, "devices", "m-audio-code61.yaml")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        # one slot: without its presets: (and modes:), Up / Down back to program changes
        text = text.split("\n# Presets (core/README.md")[0].replace("action: preset_next", "action: program_next") \
            .replace("action: preset_prev", "action: program_prev") + "\n"
        dev, diags = gd.load_device(path, text)
        bd = dev.budget
        self.assertEqual((bd["preset_values"], bd["global_values"]), (152, 13))
        # preset: 8 int8 singles (1 word), bend.glide + ramp.time (2), button.action/number 41 values
        # (1 + 2 + 11 each), analog cc and mode 14 each (1 + 1 + 4 each), encoder cc/mode 8 (1 + 1 + 2 each), pad.note 16
        # (1 + 1 + 4) = 66 group words; + 4 header, 4 name, 1 crc
        self.assertEqual((bd["preset_record"], bd["global_record"]), (4 * (9 + 66), 4 * (9 + 19)))   # globals + midi.thru, midi.din_out, midi.usb_to_din (din_midi:)
        self.assertEqual((bd["used"], bd["capacity"], bd["record_limit"]), (412, 2048 - 300, 1024))
        self.assertFalse([d for d in diags if "config page" in d.msg], [str(d) for d in diags])
        self.assertEqual((bd["max_slots"], bd["max_slots_calm"]), (5, 4))
        with_slots = lambda n: text + f"\npresets: {{ slots: {n} }}\n"   # noqa: E731
        dev2, _ = load(with_slots(2))
        self.assertEqual((dev2.budget["preset_record"], dev2.budget["used"], dev2.budget["capacity"]),
                         (4 * (9 + 67), 724, 2048 - 304))  # + program
        for n in (2, 3, 4):           # the Code 61's 3 slots: 1024 of 1744 B (59%), was 99% of one page
            self.assertFalse([d for d in self.errs(with_slots(n), level="warning") if "config page" in d.msg], n)
        w = [str(d) for d in self.errs(with_slots(5), level="warning") if "config page" in d.msg]
        self.assertTrue(w and "presets use 93% of the stm32f1 config pages" in w[0] and
                        "1636 of 1744 B (3 pages of 1024 B, less the room one compaction needs)" in w[0] and
                        "Up to 5 slots fit; 4 or fewer stay under 85%" in w[0], w)
        e = self.assertError(with_slots(6), r"presets don't fit the stm32f1 config pages", path="presets.slots")
        self.assertIn("1940 of 1744 B", e.msg)
        self.assertIn("At most 5 slots fit", e.msg)
        # the NUC keeps its 2-word entries
        self.assertEqual(gd.STORE_BUDGETS["nuvoton-nuc1xx"].get("format"), None)

    def test_wide_params(self):
        """WIDE_PARAMS (the packed record's int16 values) are exactly the stored params whose range in
        core/src/settings.c leaves -128..127."""
        with open(os.path.join(CORE_SRC, "settings.c"), encoding="utf-8") as f:
            src = f.read()
        wide, names = set(), set()
        for body in re.findall(r"\{ \.id = P_\w+,(.*?)\},", src, re.S):
            name = re.search(r'\.name = "([^"]+)"', body).group(1)
            names.add(name)
            lo, hi = (re.search(rf"\.{k} = ([^,]+),", body).group(1).strip() for k in ("min", "max"))
            # symbolic bounds: N_ACTIONS - 1 (< 20) and GM_N_PRESETS (<= MAX_PRESETS) fit a byte
            num = lambda x: int(x) if re.fullmatch(r"-?\d+", x) else 0   # noqa: E731
            self.assertLessEqual(gd.MAX_PRESETS, 127)
            if not (-128 <= num(lo) <= 127 and -128 <= num(hi) <= 127):
                wide.add(name)
        self.assertEqual(names, set(gd.PARAM_ID))
        stored = {n for n, _i, s, _c in gd.PARAMS if s != gd.VOLATILE}
        self.assertEqual(wide & stored, gd.WIDE_PARAMS)

    def test_keystation_presets(self):
        dev, diags = gd.load_device(KEYSTATION)
        self.assertFalse(gd.errors(diags), [str(d) for d in diags])
        self.assertEqual(dev.n_presets, 8)
        self.assertEqual([p.name for p in dev.presets[:3]], ["Default", "Drums", "DAW"])
        # the components' defaults are unchanged by the presets
        self.assertEqual([b.action_name for b in dev.buttons],
                         ["cc_momentary", "none", "octave_down", "octave_up", "bend_down", "bend_up", "cc_ramp"])
        self.assertIn((1, 0, 10, "midi.channel = 10"), dev.presets[1].vals)
        bd = dev.budget
        self.assertEqual((bd["preset_record"], bd["global_record"], bd["used"], bd["capacity"]), (228, 84, 5, 5))
        self.assertEqual(bd["max_slots"], 9)


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestWriteToFlash(unittest.TestCase):
    """write_to_flash: what the device stores on its own (core/README.md); off unless given."""
    errs = TestValidation.errs
    assertError = TestValidation.assertError
    assertClean = TestValidation.assertClean
    gen = TestPresets.gen
    warns = TestPresets.warns
    BASE = presets("slots: 3")
    LINE = BASE.count("\n") + 1     # write_to_flash:'s line

    def defines(self, text):
        """(GM_WRITE_LAST_PRESET, GM_WRITE_DELAY_MS, GM_WRITE_IDLE_MS) as generated."""
        _dev, h, _c = self.gen(text)
        d = dict(re.findall(r"#define GM_WRITE_(\w+) (\d+)u? ", h))
        return int(d["LAST_PRESET"]), int(d["DELAY_MS"]), int(d["IDLE_MS"])

    def test_default_off(self):
        self.assertEqual(self.defines(self.BASE), (0, 10000, 3000))
        self.assertEqual(self.defines(self.BASE + "write_to_flash: false\n"), (0, 10000, 3000))
        self.assertEqual(self.defines(self.BASE + "write_to_flash:\n"), (0, 10000, 3000))
        self.assertEqual(self.warns(self.BASE + "write_to_flash: false\n"), [])
        _dev, h, _c = self.gen(self.BASE)
        self.assertIn("#define GM_WRITE_LAST_PRESET 0 /* write_to_flash.last_preset */", h)

    def test_shorthand_true(self):
        self.assertEqual(self.defines(self.BASE + "write_to_flash: true\n"), (1, 10000, 3000))
        self.assertEqual(self.warns(self.BASE + "write_to_flash: true\n"), [])

    def test_mapping(self):
        on = "write_to_flash: { last_preset: true, delay: 30s, idle: 500ms }\n"
        self.assertEqual(self.defines(self.BASE + on), (1, 30000, 500))
        self.assertEqual(self.defines(self.BASE + "write_to_flash:\n  last_preset: true\n  delay: 1min\n"),
                         (1, 60000, 3000))
        self.assertEqual(self.defines(self.BASE + "write_to_flash: { last_preset: false }\n")[0], 0)
        self.assertEqual(self.defines(self.BASE + "write_to_flash: {}\n")[0], 0)
        # timings without anything to store: a warning, on their line
        w = [d for d in self.errs(self.BASE + "write_to_flash:\n  delay: 5s\n", level="warning")]
        self.assertEqual([(d.path, d.line) for d in w], [("write_to_flash.delay", self.LINE + 1)],
                         [str(d) for d in w])
        self.assertIn("last_preset: true", w[0].msg)

    def test_bad_values(self):
        b = self.BASE
        self.assertError(b + "write_to_flash: yes please\n", r"must be true, false or a mapping", line=self.LINE,
                         path="write_to_flash")
        self.assertError(b + "write_to_flash: [last_preset]\n",
                         r"must be true, false or a mapping like `\{ last_preset: true \}` \(got list\)")
        self.assertError(b + "write_to_flash: { last_preset: 1 }\n", r"must be true or false",
                         path="write_to_flash.last_preset")
        self.assertError(b + "write_to_flash:\n  last_preset: true\n  delay: 10\n", r"needs a unit", line=self.LINE + 2,
                         path="write_to_flash.delay")
        self.assertError(b + "write_to_flash: { last_preset: true, delay: 500ms }\n", r"500ms is out of range \(1s..1h\)")
        self.assertError(b + "write_to_flash: { last_preset: true, delay: 2h }\n", r"must be a time period")
        self.assertError(b + "write_to_flash: { last_preset: true, delay: 61min }\n", r"out of range \(1s..1h\)")
        self.assertError(b + "write_to_flash: { last_preset: true, idle: 50ms }\n",
                         r"50ms is out of range \(100ms..10min\)", path="write_to_flash.idle")
        self.assertError(b + "write_to_flash: { last_preset: true, idle: soon }\n", r"must be a time period like 10s")

    def test_unknown_keys(self):
        e = self.assertError(self.BASE + "write_to_flash:\n  last_presets: true\n", r"unknown key",
                             line=self.LINE + 1, path="write_to_flash.last_presets")
        self.assertIn("did you mean 'last_preset'", e.msg)
        self.assertError(self.BASE + "write_to_flash: { settings: true }\n", r"unknown key \(known: last_preset, "
                                                                              r"delay, idle\)")
        self.assertError(self.BASE + "write_to_flas: true\n", r"did you mean 'write_to_flash'")

    def test_one_preset(self):
        """Nothing to remember: a warning, and the code is left out."""
        text = MINIMAL_NUC + "write_to_flash: true\n"
        self.assertTrue(any("one preset" in w for w in self.warns(text)))
        self.assertEqual(self.defines(text)[0], 0)

    def test_devices_leave_it_off(self):
        for name in sorted(os.listdir(os.path.join(REPO, "devices"))):
            if name.endswith(".yaml"):
                with self.subTest(name):
                    dev, diags = gd.load_device(os.path.join(REPO, "devices", name))
                    self.assertIsNotNone(dev, [str(d) for d in diags])
                    self.assertFalse(dev.write_to_flash["last_preset"])


@unittest.skipUnless(GCC and gd.yaml, "needs host gcc and PyYAML")
class TestPresetsCompile(unittest.TestCase):
    setUp = TestGenerateCompiles.setUp
    tearDown = TestGenerateCompiles.tearDown
    gcc = TestGenerateCompiles.gcc
    dump = TestGenerateCompiles.dump

    def test_presets_dump(self):
        dev, diags = load(presets("""
slots: 4
revision: 7
list:
  - { name: Main, program: 5 }
  - name: "Q\\"uote?"
    midi: { channel: 3 }
    buttons: { c: { action: preset_select, preset: 4 } }
    encoders: { enc: { mode: relative_twos } }
"""))
        self.assertIsNotNone(dev, [str(d) for d in diags])
        out = self.dump(gen_dir(dev, self.tmp), "presets")
        self.assertIn("presets=4 revision=7", out)
        self.assertIn(" preset 1 name=Main program=5 n=0 vals=null", out)
        self.assertIn(' preset 2 name=Q"uote? program=-1 n=4 vals=ptr\n'
                      "  val id=1 index=0 value=3\n  val id=9 index=2 value=15\n"
                      "  val id=10 index=2 value=4\n  val id=16 index=0 value=1\n", out)
        self.assertIn(" preset 4 name=Preset 4 program=-1 n=0 vals=null", out)

    def test_no_presets_dump(self):
        dev, _ = load(MINIMAL_NUC)
        out = self.dump(gen_dir(dev, self.tmp), "nopresets")
        self.assertIn("presets=1 revision=0\n preset 1 name=Default program=-1 n=0 vals=null\n", out)

    def test_keystation_dump(self):
        dev, _ = gd.load_device(KEYSTATION)
        out = self.dump(gen_dir(dev, self.tmp), "ks")
        self.assertIn("presets=8 revision=0", out)
        self.assertIn(" preset 2 name=Drums program=-1 n=6 vals=ptr", out)


# ---- modes (research/core/presets-design.md §6, core/README.md "Modes") ----

MODES_NUC = MINIMAL_NUC + """\
  - { id: menu, pin: PA2, led: menu_led }
  - { id: other, pin: PA3, action: note, note: 60 }
leds:
  - { id: menu_led, pin: PA12 }
  - { id: lamp, pin: PA13 }
key_matrix:
  type: velocity
  rows: [PA8, PA9, PC0, PC1, PC2, PC3, PC6, PC7]
  columns: [PB0, PB1, PB2, PB3, PB4, PB5, PB8, PB12]
  keys: 32
  first_note: 48
presets: { slots: 4 }
"""


def modes(body, base=MODES_NUC):
    """base + a modes: block (body: the lines under it, indented by 2)."""
    return base + "modes:\n" + "".join(f"  {ln}\n" for ln in body.strip("\n").split("\n"))


ROOT = "- id: menu\n  toggle: { button: menu }\n  keys:\n"


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestModes(unittest.TestCase):
    errs = TestValidation.errs
    assertError = TestValidation.assertError
    assertClean = TestValidation.assertClean
    gen = TestPresets.gen

    def warns(self, text):
        return [d.msg for d in self.errs(text, level="warning")]

    def test_layer_and_with(self):
        base = MODES_NUC.replace(
            "  - { id: other, pin: PA3, action: note, note: 60 }\n",
            "  - { id: other, pin: PA3, action: note, note: 60 }\n"
            "  - { id: shift, pin: PA4, action: cc_momentary, cc: 5 }\n"
            "  - { id: mute, pin: PA5, action: cc_toggle, cc: 6, led: lamp, daw: mute_1 }\n"
            "  - { id: sel, pin: PA6, action: cc_momentary, cc: 8 }\n")
        arm = "- id: arm\n  toggle: { button: sel, with: shift }\n  led: menu_led\n  layer:\n"
        text = modes(arm + "    mute: { cc: 7, daw: arm_1, active: off, idle: on }\n", base)
        self.assertClean(text)
        dev, h, c = self.gen(text)
        self.assertIn("#define GM_LAYERS 1 ", h)
        # the twin: after the YAML's buttons, the mute switch and a new LED (index 2), layer 1 (mode 0 + 1),
        # base 4 (mute); its LED the lamp's pins, base LED 1
        self.assertIn('{ { 0, 0, 5 }, "Mute arm", 2, 1, 4 }, /* mute_arm: PA5 */', c)
        self.assertRegex(c, r"\{ \{ 0, 0, 13 \}, GM_NO_PIN, GM_NO_PIN, GM_LED_LOOKS\(1, 0\), 1, 1 \}, /\* lamp_arm")
        self.assertIn('{ "mute_arm", "Mute arm", "arm_1" }', c)
        # it keeps cc_toggle (action 2), with CC 7; sel (5) toggles with shift (3 + 1) held, sel keeps CC 8
        self.assertIn("gm_button_action_defaults[GM_N_BUTTONS] = { 1, 0, 4, 1, 2, 1, 2 }", c)
        self.assertIn("gm_button_number_defaults[GM_N_BUTTONS] = { 1, 0, 60, 5, 6, 8, 7 }", c)
        self.assertIn("{ 5, GM_MODE_NONE, 0, 0, 0, -1, -1, 0, 0, 0, 0, 0, 4, 0, 0x00, \"arm\" }, /* arm */", c)
        self.assertNotIn("GM_LAYERS", self.gen(MODES_NUC)[1])
        # checks
        self.assertError(modes("- id: arm\n  toggle: { button: sel }\n  layer: { mute: { cc: 7 } }\n", base),
                         "its own action would never run")
        self.assertError(modes("- id: arm\n  hold: { button: menu, with: shift }\n  layer: { mute: { cc: 7 } }\n",
                               base), "unknown key")
        self.assertClean(modes(arm + "    sel: { cc: 9 }\n", base))         # a with: toggle may have a twin
        self.assertError(modes(arm + "    shift: { cc: 9 }\n", base), "enters a mode")   # its with: button not
        self.assertError(modes(arm + "    menu: { cc: 9 }\n- id: m2\n  toggle: { button: menu }\n", base),
                         "enters a mode")                                          # nor a plain toggle
        self.assertError(modes(arm + "    mute: { cc: 7 }\n  buttons: { mute: channel_up }\n", base),
                         "also in this mode's buttons")
        self.assertError(modes(arm + "    nope: { cc: 7 }\n", base), "no button, pad or encoder with id 'nope'")
        self.assertError(modes(arm + "    lamp: { cc: 7 }\n", base), r"'lamp'.*it is the id of leds\[1\] \(an LED\)")
        self.assertError(modes(arm + "    mute: { cc: 7, active: purple }\n", base), "unknown look 'purple'")
        self.assertError(modes(ROOT + "    E3: { mode: pick }\n- id: pick\n  parent: menu\n  keys: { C3: octave_up }\n"
                               "  layer: { mute: { cc: 7 } }\n", base), "only a root mode")
        self.assertTrue(any("has no LED" in w for w in self.warns(modes(arm + "    other: { note: 61, active: on }\n",
                                                                        base))))

    LAYER_BASE = MODES_NUC.replace(
        "  - { id: other, pin: PA3, action: note, note: 60 }\n",
        "  - { id: other, pin: PA3, action: note, note: 60 }\n"
        "  - { id: shift, pin: PA4, action: cc_momentary, cc: 5, daw: shift }\n"
        "  - { id: sel, pin: PA6, action: cc_momentary, cc: 8, led: sel_led }\n").replace(
        "  - { id: lamp, pin: PA13 }\n",
        "  - { id: lamp, pin: PA13 }\n  - { id: sel_led, pin: PA14 }\n  - { id: pad_led, pin: PA15, pin2: PB13 }\n") + \
        "pads:\n  - { id: kick, pin: PA7, note: 36, led: pad_led }\n  - { id: snare, pin: PA5, note: 38 }\n" \
        "encoders:\n  - { id: e1, pin_a: PB6, pin_b: PB7, cc: 20, daw: device_param_1 }\n" \
        "  - { id: e2, pin_a: PB9, pin_b: PB10, cc: 21 }\n"
    GRID = "- id: grid\n  toggle: { button: sel, with: shift }\n  layer:\n"

    def test_label_tables(self):
        """The settings' index labels: the components' names, as gm_device has them, in flash (const tables);
        pad and mode names only with pads / modes."""
        text = modes(self.GRID + "    kick: { daw: clip_1_1 }\n    e2: { name: Pan 2 }\n", self.LAYER_BASE)
        dev, h, c = self.gen(text)
        names = lambda xs: ", ".join(gd.c_str(x.name) for x in xs)   # noqa: E731
        self.assertIn(f"const char *const gm_button_names[GM_N_BUTTONS] = {{ {names(dev.buttons)} }};", c)
        self.assertIn('const char *const gm_encoder_names[GM_N_ENCODERS] = { "E1", "E2", "Pan 2" };', c)
        self.assertIn('const char *const gm_pad_names[GM_N_PADS] = { "Kick", "Snare" };', c)   # not the twins
        self.assertIn('const char *const gm_mode_names[GM_N_MODES] = { "grid" };', c)
        self.assertIn("const char *const gm_analog_names[1] = { 0 };", c)                     # no analogs
        for b in dev.buttons:                                                                  # what gm_device has
            self.assertIn(f", {gd.c_str(b.name)}, ", c)
        _, _, c = self.gen(MINIMAL_NUC)
        self.assertIn('const char *const gm_button_names[GM_N_BUTTONS] = { "A" };', c)
        self.assertNotIn("gm_pad_names", c)
        self.assertNotIn("gm_mode_names", c)

    def test_layer_pads_and_encoders(self):
        """Pad and encoder twins, and a twin of the with: toggle itself (core/README.md "Button layers")."""
        text = modes(self.GRID + "    kick: { daw: clip_1_1, idle: off, active: colour2 }\n    snare: { daw: clip_2_1 }\n"
                     "    e1: { cc: 41, daw: pan_1 }\n    e2: { name: Pan 2 }\n    sel: { daw: grid_left }\n",
                     self.LAYER_BASE)
        self.assertClean(text)
        dev, h, c = self.gen(text)
        self.assertIn("#define GM_LAYERS 5 ", h)               # sel, kick, snare, e1, e2
        self.assertIn("#define GM_ENC_TWINS 2 ", h)
        self.assertIn("#define GM_PAD_TWINS 2 ", h)
        self.assertIn("#define GM_N_PADS 2 ", h)               # the twins aren't pads: no pad.note of their own
        self.assertIn("#define GM_N_ENCODERS 4", h)
        # encoder twins: the base's pins and steps, layer 1, base 0 / 1; their own cc (e2's: the base's)
        self.assertIn('{ { 0, 0, 22 }, { 0, 0, 23 }, "E1", 4, 0, 0 }, /* e1', c)
        self.assertIn('{ { 0, 0, 22 }, { 0, 0, 23 }, "E1 grid", 4, 1, 0 }, /* e1_grid', c)
        self.assertIn('{ { 0, 0, 25 }, { 0, 0, 26 }, "Pan 2", 4, 1, 1 }, /* e2_grid', c)
        self.assertIn("gm_encoder_cc_defaults[GM_N_ENCODERS] = { 20, 21, 41, 21 }", c)
        self.assertIn('{ "e1_grid", "E1 grid", "pan_1" }', c)
        # pad twins: { layer, base, LED }: kick's twin LED is the next LED, with its looks; snare has none
        led = next(i for i, x in enumerate(dev.leds) if x.id == "pad_led_grid")
        self.assertEqual((dev.leds[led].layer, dev.leds[led].base, dev.leds[led].looks), (1, 3, (0, 2)))
        self.assertIn(f"    {{ 1, 0, {led} }}, /* kick_grid */", c)
        self.assertIn("    { 1, 1, -1 }, /* snare_grid */", c)
        self.assertIn('const gm_comp_t gm_comp_pads[GM_N_PADS + GM_PAD_TWINS] = { { "kick", "Kick", "" }, '
                      '{ "snare", "Snare", "" }, { "kick_grid", "Kick grid", "clip_1_1" }, '
                      '{ "snare_grid", "Snare grid", "clip_2_1" } };', c)
        # the with: toggle's twin: sel's switch, its action and CC, and a twin of its LED
        sel = next(b for b in dev.buttons if b.id == "sel_grid")
        self.assertEqual((sel.action_name, sel.number, sel.daw, sel.base), ("cc_momentary", 8, "grid_left", 4))
        # stored: the twins' settings are globals (layer.*), once, not per preset; pad.note of the 2 pads only
        n_preset, n_global = gd.stored_counts(dev)
        dev0, _ = load(self.LAYER_BASE)
        self.assertEqual(n_preset, gd.stored_counts(dev0)[0])
        # layer.enabled (1 mode), layer.button.action / number (1 twin), layer.encoder.cc / mode (2 twins)
        self.assertEqual(n_global - gd.stored_counts(dev0)[1], 1 + 2 + 4)
        # a preset can't set a twin's settings (global); a pad twin has no note
        pre = "presets:\n  slots: 4\n  list:\n    - name: A\n    - name: B\n      %s\n"
        self.assertError(text.replace("presets: { slots: 4 }\n", pre % "encoders: { e1_grid: { cc: 50 } }"),
                         r"'e1_grid' is a layer twin: its settings are global \(layer.encoder.\*\)")
        self.assertError(text.replace("presets: { slots: 4 }\n", pre % "buttons: { sel_grid: { cc: 50 } }"),
                         r"'sel_grid' is a layer twin: its settings are global \(layer.button.\*\)")
        self.assertError(text.replace("presets: { slots: 4 }\n", pre % "pads: { kick_grid: { note: 50 } }"),
                         r"'kick_grid' is the id of .*a pad twin")
        self.assertNotIn("gm_pad_twins", self.gen(self.LAYER_BASE)[2])
        self.assertNotIn("GM_ENC_TWINS", self.gen(self.LAYER_BASE)[1])
        # checks
        bad = lambda body: modes(self.GRID + body, self.LAYER_BASE)    # noqa: E731
        self.assertError(bad("    kick: { cc: 5 }\n"), r"unknown key \(known: name, daw, idle, active\)")
        self.assertError(bad("    e1: { action: note }\n"), r"unknown key \(known: name, cc, daw\)")
        self.assertError(bad("    e1: { cc: 128 }\n"), r"128 is out of range")
        self.assertError(bad("    kick: { active: purple }\n"), "unknown look 'purple'")
        self.assertTrue(any("has no LED" in w for w in self.warns(bad("    snare: { active: on }\n"))))
        ring = self.LAYER_BASE.replace("cc: 20, daw: device_param_1 }", "cc: 20, ring: [lamp] }")
        self.assertError(modes(self.GRID + "    e1: { cc: 41 }\n", ring), "a ring encoder can't have a layer twin yet")
        twice = modes(self.GRID + "    kick: {}\n- id: g2\n  toggle: { button: menu }\n  layer: { kick_grid: {} }\n",
                      self.LAYER_BASE)
        self.assertError(twice, "is a layer twin: give the layer the pad itself")
        self.assertError(modes(self.GRID + "    e1: {}\n- id: g2\n  toggle: { button: menu }\n  layer: { e1_grid: {} }\n",
                               self.LAYER_BASE), "is a layer twin: give the layer the encoder itself")

    def test_layer_compiles(self):
        if not GCC:
            self.skipTest("needs host gcc")
        tmp = tempfile.mkdtemp()
        try:
            dev, diags = load(modes(self.GRID + "    kick: { daw: clip_1_1 }\n    e1: { cc: 41 }\n    sel: {}\n",
                                    self.LAYER_BASE.replace("cc: 21 }", "cc: 21 }\nanalog:\n  - { id: f, pin: PA0, "
                                                                        "cc: 7, touch: movement }")))
            self.assertIsNotNone(dev, [str(d) for d in diags])
            TestGenerateCompiles.compile_with_core(self, gen_dir(dev, tmp))
        finally:
            shutil.rmtree(tmp)

    CYCLE_BASE = LAYER_BASE.replace("  - { id: lamp, pin: PA13 }\n",
                                    "  - { id: lamp, pin: PA13 }\n  - { id: rgb, pin: PC8, pin2: PC9, pin3: PC10 }\n")
    CYCLE = ("- id: encs\n  cycle: { button: sel, with: shift }\n  led: rgb\n  steps:\n"
             "    - { id: plain, name: MIDI, colour: blue }\n"
             "    - id: dev\n      name: DEVICE\n      colour: green\n      layer: { e1: { cc: none, daw: device_param_1 }, "
             "kick: { daw: clip_1_1 } }\n"
             "    - { id: pan, colour: red, enabled: false, layer: { e1: { cc: 41, daw: pan_1 }, e2: { cc: none } } }\n")

    def test_cycle(self):
        """cycle: + steps: (core/README.md "Cycles"): one root mode per step, the cycle's button on each."""
        text = modes(self.CYCLE + "- id: arm\n  toggle: { button: menu }\n  layer: { e2: { cc: 50 } }\n",
                     self.CYCLE_BASE)
        self.assertClean(text)
        dev, h, c = self.gen(text)
        self.assertEqual([m.id for m in dev.modes], ["plain", "dev", "pan", "arm"])
        self.assertIn("#define GM_N_CYCLES 1 ", h)
        self.assertIn("#define GM_MODES_ENABLED 1, 1, 0, 1 ", h)
        self.assertIn("#define GM_ENC_TWINS 4 ", h)
        # steps: toggle sel (4), with shift (3 + 1), root = itself, the cycle's LED (rgb, 2), cycle 1, colour
        # 0x80 | the pins (blue 4, green 2, red 1), the name (or the id)
        self.assertIn('{ 4, GM_MODE_NONE, 0, 2, 0, -1, -1, 0, 0, 0, 0, 0, 4, 1, 0x84, "MIDI" }, /* plain */', c)
        self.assertIn('{ 4, GM_MODE_NONE, 1, 2, 0, -1, -1, 0, 0, 0, 0, 0, 4, 1, 0x82, "DEVICE" }, /* dev */', c)
        self.assertIn('{ 4, GM_MODE_NONE, 2, 2, 0, -1, -1, 0, 0, 0, 0, 0, 4, 1, 0x81, "pan" }, /* pan */', c)
        self.assertIn('{ 1, GM_MODE_NONE, 3, 0, 0, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0x00, "arm" }, /* arm */', c)
        # twins: layer = step index + 1; cc: none = -1
        self.assertIn('"E1 dev", 4, 2, 0 }', c)
        self.assertIn('"E1 pan", 4, 3, 0 }', c)
        self.assertIn("gm_encoder_cc_defaults[GM_N_ENCODERS] = { 20, 21, -1, 41, -1, 50 }", c)
        # params: the YAML's own components per preset, the layer.* globals (counted once in the budget)
        counts = {n: cnt(dev) for n, _i, _s, cnt in gd.PARAMS}
        self.assertEqual((counts["encoder.cc"], counts["layer.encoder.cc"], counts["layer.encoder.mode"]), (2, 4, 4))
        self.assertEqual((counts["button.action"], counts["layer.button.action"], counts["layer.enabled"]), (5, 0, 4))
        self.assertEqual([gd.PARAM_ID[n] for n in ("layer.enabled", "layer.button.action", "layer.button.number",
                                                   "layer.encoder.cc", "layer.encoder.mode")], [34, 35, 36, 37, 38])
        dev0, _ = load(self.CYCLE_BASE)
        b, b0 = gd.storage_budget(dev), gd.storage_budget(dev0)
        self.assertEqual(b["preset_values"], b0["preset_values"])          # no twin value per preset
        self.assertEqual(b["global_values"], b0["global_values"] + 4 + 8)  # layer.enabled + 4 encoder twins x 2
        self.assertEqual(b["worst_bytes"] - b0["worst_bytes"], b["global_record"] - b0["global_record"])
        # without twins or cycles: no layer.enabled (the Keystation's edit mode)
        self.assertEqual(next(cnt for n, _i, _s, cnt in gd.PARAMS if n == "layer.enabled")(load(modes(
            ROOT + "    C3: octave_reset\n"))[0]), 0)
        # power-on and the disabled steps are the firmware's; at least one step must be enabled
        bad = lambda body: modes(body, self.CYCLE_BASE)    # noqa: E731
        self.assertError(bad(self.CYCLE.replace("colour: blue }", "colour: blue, enabled: false }")
                             .replace("colour: green\n", "colour: green\n      enabled: false\n")),
                         "every step has enabled: false")
        two = "- id: c\n  cycle: { button: sel, with: shift }\n  steps:\n"
        self.assertError(bad(two + "    - { id: a }\n"), "1 step: a cycle has 2 to 8")
        self.assertError(bad(two + "".join(f"    - {{ id: s{k} }}\n" for k in range(9))), "9 steps: a cycle has 2 to 8")
        self.assertError(bad(two + "    - { id: a }\n    - { id: c }\n"), r"mode 'c' is already modes\[0\]")
        self.assertError(bad(two + "    - { id: a }\n    - { id: b }\n- { id: a, toggle: menu }\n"),
                         r"mode 'a' is already modes\[0\]\.steps\[0\]")
        self.assertError(bad(two + "    - { id: a, keys: { C3: octave_up } }\n    - { id: b }\n"),
                         r"unknown key .*known: id, name, colour, enabled, layer")
        self.assertError(bad("- id: c\n  cycle: { button: sel }\n  toggle: { button: menu }\n  steps:\n"
                             "    - { id: a }\n    - { id: b }\n"), "cycle: and steps: only, not toggle:")
        self.assertError(bad("- id: c\n  cycle: { button: sel, with: shift }\n"), "needs steps:")
        self.assertError(bad("- id: c\n  steps:\n    - { id: a }\n    - { id: b }\n"), "needs cycle:")
        self.assertError(bad(two + "    - { id: a }\n    - { id: b }\n  layer: { e1: {} }\n"),
                         "a cycle has no layer: of its own")
        self.assertError(bad(two + "    - { id: a }\n    - { id: b }\n- { id: m, toggle: { button: sel, with: shift } }\n"),
                         "button 'sel' already enters mode 'c'")
        self.assertError(bad("- id: c\n  cycle: { button: menu }\n  steps:\n    - { id: a }\n    - { id: b }\n"
                             "- { id: m, hold: menu }\n"), "button 'menu' already enters mode 'c'")
        self.assertError(bad("- id: c\n  cycle: { button: shift }\n  steps:\n    - { id: a }\n    - { id: b }\n"),
                         "its own action would never run")
        self.assertError(bad(two.replace("sel", "other") + "    - { id: a, colour: red }\n    - { id: b }\n"),
                         "no LED to show it")                          # other has no LED, the cycle no led:
        self.assertError(bad(self.CYCLE.replace("colour: blue", "colour: purple")), "unknown colour 'purple' for LED 'rgb'")
        self.assertError(bad(two.replace("steps:", "led: lamp\n  steps:") + "    - { id: a, name: ABCDEFGHIJKLMNOPQ }\n"
                             "    - { id: b }\n"), "at most 16 ASCII characters")
        w = self.warns(bad(two.replace("steps:", "led: lamp\n  steps:") + "    - { id: a, colour: red }\n"
                           "    - { id: b, colour: off }\n"))
        self.assertTrue(any("can't show red" in x for x in w), w)
        self.assertFalse(any("does nothing" in x for x in w), w)      # a step without a layer: the base components
        # a key can't go to a step; a step's encoder twin takes cc: none, a button twin doesn't
        self.assertError(bad(self.CYCLE.replace("e2: { cc: none }", "sel: { cc: none }")), "must be an integer")
        self.assertError(bad(self.CYCLE + "- id: m\n  toggle: { button: menu }\n  keys: { C3: { mode: plain } }\n"),
                         "mode 'plain' has no parent")

    def test_cycle_compiles(self):
        if not GCC:
            self.skipTest("needs host gcc")
        tmp = tempfile.mkdtemp()
        try:
            dev, diags = load(modes(self.CYCLE + "- id: arm\n  toggle: { button: menu }\n  layer: { e2: { cc: 50 } }\n",
                                    self.CYCLE_BASE))
            self.assertIsNotNone(dev, [str(d) for d in diags])
            TestGenerateCompiles.compile_with_core(self, gen_dir(dev, tmp))
        finally:
            shutil.rmtree(tmp)

    def test_no_modes(self):
        _dev, h, c = self.gen(MODES_NUC)
        self.assertIn("#define GM_N_MODES 0", h)
        self.assertNotIn("gm_modes", c)

    def test_generate(self):
        dev, h, c = self.gen(modes(ROOT + """\
    C3: octave_reset
    C#3: { action: octave_down }
    E3: { mode: pick }
    F3: { action: preset_select, preset: 3 }
    A3: { action: channel, channel: 10 }
    B3: { action: velocity_curve, curve: soft }
    60: transpose_up
  preset_blink: [lamp, menu_led]
- id: pick
  parent: menu
  keys: { white_from: D4, action: preset_select, C3: { mode: chan } }
- id: chan
  parent: pick
  led: { lamp: blink }
  keys: { from: C5, action: channel, count: 3 }
"""))
        self.assertIn("#define GM_N_MODES 3", h)
        self.assertIn("static const gm_mode_key_t mode_1_keys[7] = { /* menu */", c)
        rows = re.findall(r"\{ (\d+), (\d+), (\d+) \}, /\* (.*?) \*/", c.split("modes: key bindings")[1])
        self.assertEqual([(int(k), int(op), int(a), t) for k, op, a, t in rows], [
            (0, 6, 0, "C3: octave_reset"), (1, 4, 0, "C#3: octave_down"), (4, 12, 1, "E3: mode pick"),
            (5, 1, 3, "F3: preset_select 3"), (9, 10, 10, "A3: channel 10"), (11, 11, 1, "B3: velocity_curve soft"),
            (12, 8, 0, "C4: transpose_up"),
            (0, 12, 2, "C3: mode chan"), (14, 1, 1, "D4: preset_select 1"), (16, 1, 2, "E4: preset_select 2"), (17, 1, 3, "F4: preset_select 3"),
            (19, 1, 4, "G4: preset_select 4"),
            (24, 10, 1, "C5: channel 1"), (25, 10, 2, "C#5: channel 2"), (26, 10, 3, "D5: channel 3")])
        # toggle, parent, root, led, blink, count LED, done LED, n, keys
        # ..., hold, n_buttons, buttons
        self.assertIn("{ 1, GM_MODE_NONE, 0, 0, 0, 1, 0, 7, mode_1_keys, 0, 0, 0, 0, 0, 0x00, \"menu\" }, /* menu */", c)
        self.assertIn("{ GM_MODE_NONE, 0, 0, 0, 1, 1, 0, 5, mode_2_keys, 0, 0, 0, 0, 0, 0x00, \"pick\" }, /* pick */", c)   # parent's LED, blinking
        self.assertIn("{ GM_MODE_NONE, 1, 0, 1, 1, 1, 0, 3, mode_3_keys, 0, 0, 0, 0, 0, 0x00, \"chan\" }, /* chan */", c)
        self.assertEqual(dev.modes[2].root, 0)

    def test_modes_compile(self):
        if not GCC:
            self.skipTest("needs host gcc")
        tmp = tempfile.mkdtemp()
        try:
            dev, diags = load(modes(ROOT + "    E3: { mode: pick }\n- { id: pick, parent: menu, keys: { C4: preset_next } }"))
            self.assertIsNotNone(dev, [str(d) for d in diags])
            TestGenerateCompiles.compile_with_core(self, gen_dir(dev, tmp))
        finally:
            shutil.rmtree(tmp)

    gcc = TestGenerateCompiles.gcc

    def test_keystation(self):
        dev, diags = gd.load_device(KEYSTATION)
        self.assertFalse(gd.errors(diags), [str(d) for d in diags])
        self.assertEqual([m.id for m in dev.modes], ["edit", "preset_pick", "channel_pick"])
        edit = dev.modes[0]
        self.assertEqual((edit.toggle, edit.led, edit.blink, edit.show), (1, 1, False, (2, 3)))
        self.assertEqual([k[3] for k in dev.modes[1].keys][:2], ["D4: preset_select 1", "E4: preset_select 2"])
        self.assertEqual(len(dev.modes[1].keys), 8)
        self.assertEqual(len(dev.modes[2].keys), 16)

    def test_section_errors(self):
        self.assertError(modes("a: 1"), r"must be a list", path="modes")
        self.assertError(modes("- toggle: { button: menu }"), r"needs id:")
        self.assertError(modes("- { id: menu, toggle: { button: menu }, colour: red }"), r"unknown key", path="modes[0].colour")
        self.assertError(modes(ROOT + "    C3: octave_reset\n- { id: menu, parent: menu }"), r"mode 'menu' is already modes\[0\]")
        self.assertError(modes("- { id: x }"), r"needs toggle: \{ button: <id> \}.*or parent:")
        self.assertError(modes("- { id: x, toggle: { button: menu }, parent: y }"),
                         r"one of toggle:, hold: or parent:, not toggle and parent")
        self.assertError(modes("- { id: x, toggle: menu, hold: menu }"), r"not toggle and hold")
        self.assertError(modes(ROOT + "    C3: octave_reset", base=MINIMAL_NUC), r"needs a key_matrix")
        self.assertTrue(any("has no keys" in w for w in self.warns(modes("- { id: x, toggle: { button: menu } }"))))

    def test_toggle_errors(self):
        self.assertError(modes("- { id: x, toggle: { button: mneu }, keys: { C3: octave_reset } }"),
                         r"no button with id 'mneu'.*did you mean 'menu'")
        self.assertError(modes("- { id: x, toggle: { button: a }, keys: { C3: octave_reset } }"),
                         r"button 'a' toggles this mode, so its own action would never run: remove `action: cc_momentary`")
        self.assertError(modes("- { id: x, toggle: { btn: menu } }"), r"unknown key", path="modes[0].toggle.btn")
        self.assertError(modes("- { id: x, toggle: { button: menu } }\n- { id: z, toggle: menu }"),
                         r"button 'menu' already enters mode 'x'")
        self.assertClean(modes("- { id: x, toggle: menu, keys: { C3: octave_reset } }"))
        # presets can't rebind the mode button
        t = modes("- { id: x, toggle: menu, keys: { C3: octave_reset } }").replace(
            "presets: { slots: 4 }\n", "presets:\n  list:\n    - {}\n    - buttons: { menu: { action: note, note: 3 } }\n")
        self.assertError(t, r"button 'menu' toggles mode 'x', so presets can't change it",
                         path="presets.list[1].buttons.menu")

    def test_parent_errors(self):
        self.assertError(modes(ROOT + "    E3: { mode: pick }\n- { id: pick, parent: mnu, keys: { C4: preset_next } }"),
                         r"no mode with id 'mnu'.*did you mean 'menu'")
        self.assertError(modes(ROOT + "    C3: octave_reset\n- { id: pick, parent: pick }"), r"can't be its own parent")
        self.assertError(modes(ROOT + "    E3: { mode: a }\n- { id: a, parent: b, keys: { C4: { mode: b } } }\n"
                                      "- { id: b, parent: a, keys: { C4: { mode: a } } }"),
                         r"the parents go round in a loop: a -> b -> a")
        self.assertError(modes(ROOT + "    C3: octave_reset\n- { id: pick, parent: menu, keys: { C4: preset_next } }"),
                         r"mode 'pick' can't be reached: no key goes to it.*`E3: \{ mode: pick \}` to the keys of mode 'menu'")
        self.assertError(modes(ROOT + "    E3: { mode: menu }"), r"mode 'menu' has no parent: a key can only go to a sub-mode")
        self.assertError(modes(ROOT + "    E3: { mode: pikc }\n- { id: pick, parent: menu, keys: { C4: preset_next } }"),
                         r"no mode with id 'pikc'.*did you mean 'pick'")

    def test_key_errors(self):
        self.assertError(modes(ROOT + "    H3: octave_reset"), r"'H3' isn't a key: name it by its note.*C3\.\.G5")
        self.assertError(modes(ROOT + "    B2: octave_reset"), r"B2 \(note 47\) isn't on the keyboard: its keys are C3\.\.G5")
        self.assertError(modes(ROOT + "    G#5: octave_reset"), r"G#5 \(note 80\) isn't on the keyboard")
        self.assertClean(modes(ROOT + "    Db3: octave_reset\n    G5: octave_up\n    49: octave_down".replace("49", "50")))
        self.assertError(modes(ROOT + "    C#3: octave_reset\n    Db3: octave_up"),
                         r"C#3 is bound twice in this mode \(octave_reset at line \d+, and octave_up\)")
        self.assertError(modes(ROOT + "    C3: octave_rest"), r"unknown action 'octave_rest' \(did you mean 'octave_reset'")
        self.assertError(modes(ROOT + "    C3: cc_toggle"), r"valid in a mode: preset_select")
        self.assertError(modes(ROOT + "    C3: preset_select"), r"preset_select needs preset: \(the slot, 1\.\.4\)")
        self.assertError(modes(ROOT + "    C3: { action: preset_select, preset: 5 }"), r"5 is out of range \(1\.\.4\)")
        self.assertError(modes(ROOT + "    C3: { action: channel, channel: 17 }"), r"17 is out of range \(1\.\.16\)")
        self.assertError(modes(ROOT + "    C3: { action: velocity_curve, curve: hrad }"), r"did you mean 'hard'")
        self.assertError(modes(ROOT + "    C3: { action: octave_up, preset: 2 }"), r"octave_up takes nothing else, not preset:")
        self.assertError(modes(ROOT + "    C3: { action: channel, preset: 2 }"), r"channel takes channel:, not preset:")
        self.assertError(modes(ROOT + "    C3: { mode: x, action: octave_up }\n- { id: x, parent: menu }"),
                         r"goes to a sub-mode does nothing else")
        self.assertError(modes(ROOT + "    C3: 5"), r"must be an action")
        nopre = MODES_NUC.replace("presets: { slots: 4 }\n", "")
        self.assertError(modes(ROOT + "    C3: preset_next", base=nopre), r"preset_next switches presets.*1 preset slot")
        sw = MODES_NUC.replace("type: velocity", "type: switch").replace("columns: [PB0, PB1, PB2, PB3, PB4, PB5, PB8, PB12]",
                                                                        "columns: [PB0, PB1, PB2, PB3]")
        self.assertError(modes(ROOT + "    C3: { action: velocity_curve, curve: hard }", base=sw), r"needs a velocity key matrix")

    def test_range_errors(self):
        self.assertError(modes(ROOT.replace("  keys:\n", "  keys: { white_from: C#4, action: preset_select }")),
                         r"C#4 is a black key")
        self.assertError(modes(ROOT.replace("  keys:\n", "  keys: { white_from: E5, action: channel }")),
                         r"16 white keys from E5 go past the top key \(G5\): 3 fit \(set count: 3")
        self.assertError(modes(ROOT.replace("  keys:\n", "  keys: { white_from: E5 }")), r"a range of keys needs")
        self.assertError(modes(ROOT.replace("  keys:\n", "  keys: { from: E5, white_from: E5, action: channel }")),
                         r"a range of keys needs")
        self.assertError(modes(ROOT.replace("  keys:\n", "  keys: { from: C4, action: octave_up }")),
                         r"a range numbers its keys 1, 2, \.\.\.: action is one of preset_select, channel")
        self.assertError(modes(ROOT.replace("  keys:\n", "  keys: { from: C4, action: preset_select, count: 5 }")),
                         r"5 is out of range \(1\.\.4\)")
        self.assertError(modes(ROOT.replace("  keys:\n", "  keys: { from: C4, action: channel, C4: octave_up }")),
                         r"C4 is bound twice")
        self.assertClean(modes(ROOT.replace("  keys:\n", "  keys: { from: C4, action: channel, C3: octave_up }")))

    def test_hold_and_buttons(self):
        dev, h, c = self.gen(modes("""\
- id: hold
  hold: { button: menu }
  buttons: { other: { action: channel, channel: 3 }, a: preset_next }
- id: edit
  toggle: lamp_btn
  buttons: { a: channel_down }
""", base=MODES_NUC.replace("leds:\n", "  - { id: lamp_btn, pin: PA4 }\nleds:\n")))
        self.assertIn("#define GM_N_MODES 2", h)
        self.assertIn("static const gm_mode_key_t mode_1_buttons[2] = { /* hold: buttons */", c)
        rows = re.findall(r"\{ (\d+), (\d+), (\d+) \}, /\* (.*?) \*/", c.split("modes: key bindings")[1])
        self.assertEqual([(int(k), int(op), int(a), t) for k, op, a, t in rows], [
            (0, 2, 0, "a: preset_next"), (2, 10, 3, "other: channel 3"), (0, 14, 0, "a: channel_down")])
        # the hold button's LED is lit while it is held; hold = 1; no keys: the keyboard plays
        self.assertIn("{ 1, GM_MODE_NONE, 0, 0, 0, -1, -1, 0, 0, 1, 2, mode_1_buttons, 0, 0, 0x00, \"hold\" }, /* hold */", c)
        self.assertIn("{ 3, GM_MODE_NONE, 1, -1, 0, -1, -1, 0, 0, 0, 1, mode_2_buttons, 0, 0, 0x00, \"edit\" }, /* edit */", c)
        self.assertEqual(self.warns(modes("- { id: x, hold: menu, buttons: { a: channel_up } }")), [])
        self.assertTrue(any("has no keys, buttons or layer" in w for w in self.warns(modes("- { id: x, hold: menu }"))))

    def test_hold_and_buttons_errors(self):
        self.assertError(modes("- { id: x, hold: { button: a }, buttons: { other: channel_up } }"),
                         r"button 'a' holds this mode, so its own action would never run")
        self.assertError(modes("- { id: x, hold: menu, keys: { E3: { mode: sub } } }\n- { id: sub, parent: x, keys: { C4: preset_next } }"),
                         r"mode 'x' is a hold mode .* can't have sub-modes", path="modes[1].parent")
        self.assertError(modes("- { id: x, hold: menu, buttons: { c: channel_up } }"), r"no button with id 'c'")
        self.assertError(modes("- { id: x, hold: menu, buttons: { menu: channel_up } }"),
                         r"button 'menu' enters a mode \(toggle:/hold:\): it can't also be bound in one")
        self.assertError(modes("- { id: x, hold: menu, buttons: { a: chanel_up } }"), r"did you mean 'channel_up'")
        self.assertError(modes("- { id: x, hold: menu, buttons: { a: { mode: x } } }"), r"buttons can't go to a sub-mode")
        self.assertError(modes("- { id: x, hold: menu, buttons: [a] }"), r"must be a mapping of button ids")
        nopre = MODES_NUC.replace("presets: { slots: 4 }\n", "")
        self.assertError(modes("- { id: x, hold: menu, buttons: { a: preset_next } }", base=nopre),
                         r"preset_next switches presets.*1 preset slot")

    def test_code61(self):
        dev, diags = gd.load_device(os.path.join(REPO, "devices", "m-audio-code61.yaml"))
        self.assertFalse(gd.errors(diags), [str(d) for d in diags])
        m = dev.modes[0]
        self.assertEqual((m.id, m.hold, dev.buttons[m.toggle].id), ("channel", True, "channel"))
        self.assertEqual([(dev.buttons[b[0]].id, b[3]) for b in m.buttons],
                         [("up", "up: channel_up"), ("down", "down: channel_down")])
        self.assertEqual(dev.n_presets, 4)

    def test_led_errors(self):
        base = ROOT + "    C3: octave_reset\n"
        self.assertError(modes(base + "  led: lmap"), r"no LED with id 'lmap'.*did you mean 'lamp'")
        self.assertError(modes(base + "  led: { lamp: fast }"), r"must be blink or on")
        self.assertError(modes(base + "  led: { lamp: blink, menu_led: on }"), r"must be an LED id")
        self.assertError(modes(base + "  preset_blink: [lamp, menu_led, lamp]"), r"needs one or two LED ids")
        self.assertError(modes(base + "  preset_blink: [lmp]"), r"no LED with id 'lmp'")
        _dev, _h, c = self.gen(modes(base + "  led: { lamp: on }\n  preset_blink: [lamp]"))
        self.assertIn("{ 1, GM_MODE_NONE, 0, 1, 0, 1, -1, 1, mode_1_keys, 0, 0, 0, 0, 0, 0x00, \"menu\" }", c)
        nopre = MODES_NUC.replace("presets: { slots: 4 }\n", "")
        self.assertTrue(any("1 preset slot" in w for w in self.warns(modes(base + "  preset_blink: [lamp]", base=nopre))))


PADS_STM = """\
gemifi: 2
name: test
board_id: 0x3F05
platform: stm32f1
chip: STM32F103RB
usb: { vid: 0x1209, pid: 0x0002 }
cd74hc4051:
  - { id: m, pin_s0: PC0, pin_s1: PC1, pin_s2: PC2, common_pin: PA0 }
pads:
  - { id: kick, pin: { cd74hc4051: m, number: 0 }, note: 36 }
  - { id: snare, pin: PA1, note: 38 }
"""


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestPads(unittest.TestCase):
    errs = TestValidation.errs
    assertError = TestValidation.assertError
    assertClean = TestValidation.assertClean

    def gen(self, text):
        dev, diags = load(text)
        self.assertIsNotNone(dev, [str(d) for d in diags])
        return dev, gd.generate(dev, "t.yaml")

    def warns(self, text):
        return [str(d) for d in self.errs(text, level="warning")]

    def test_generate(self):
        dev, files = self.gen(PADS_STM)
        h, c = files["gm_device_config.h"], files["gm_device_config.c"]
        self.assertIn("#define GM_N_PADS 2 ", h)
        for d in ("CHANNEL_DEFAULT 0", "CURVE_DEFAULT 0 /* normal */", "THRESHOLD_DEFAULT 200",
                  "RELEASE_DEFAULT 100", "FULL_DEFAULT 3000", "WINDOW_DEFAULT 3 /* ms */", "RETRIGGER_DEFAULT 20"):
            self.assertIn(f"#define GM_PAD_{d}", h)
        self.assertIn('{ { 1, 0, 0 }, "Kick", -1 }, /* kick: m.0 */', c)
        self.assertIn('{ { 0, 0, 1 }, "Snare", -1 }, /* snare: PA1 */', c)
        self.assertIn("gm_pad_note_defaults[GM_N_PADS] = { 36, 38 };", c)
        # stored values: 2 notes + channel, curve, aftertouch per preset; 5 sensing globals
        n_preset, n_global = gd.stored_counts(dev)
        dev0, _ = load(PADS_STM.split("pads:")[0] + "analog: [{ id: k, pin: PA1, cc: 1 }]\n")
        p0, g0 = gd.stored_counts(dev0)
        self.assertEqual((n_preset - p0, n_global - g0), (5 - 2, 5))   # analog.cc and .mode of dev0
        _, files = self.gen(MINIMAL_NUC)
        self.assertIn("#define GM_N_PADS 0", files["gm_device_config.h"])
        self.assertNotIn("GM_PAD_", files["gm_device_config.h"])
        self.assertNotIn("gm_pads", files["gm_device_config.c"])

    def test_pad_led(self):
        text = PADS_STM.replace("note: 36 }", "note: 36, led: kl }") + \
            "leds:\n  - { id: kl, pin: PB5 }\n"
        _, files = self.gen(text)
        self.assertIn('{ { 1, 0, 0 }, "Kick", 0 }, /* kick: m.0 */', files["gm_device_config.c"])
        self.assertError(PADS_STM.replace("note: 36 }", "note: 36, led: nope }"), r"nope")

    def test_settings(self):
        _, files = self.gen(PADS_STM + "pad_settings: { channel: 10, curve: hard, aftertouch: true, threshold: 400, "
                                       "release: 250, full: 3900, window: 5ms, retrigger: 0ms }\n")
        h = files["gm_device_config.h"]
        for d in ("CHANNEL_DEFAULT 10", "CURVE_DEFAULT 2 /* hard */", "AFTERTOUCH_DEFAULT 1", "THRESHOLD_DEFAULT 400",
                  "RELEASE_DEFAULT 250", "FULL_DEFAULT 3900", "WINDOW_DEFAULT 5 ", "RETRIGGER_DEFAULT 0 "):
            self.assertIn(f"#define GM_PAD_{d}", h)
        bad = PADS_STM + "pad_settings: { %s }\n"
        self.assertError(bad % "threshold: 50", r"needs release < threshold < full \(got release 100, threshold 50")
        self.assertError(bad % "full: 150", r"release < threshold < full")
        self.assertError(bad % "window: 3", r"needs a unit")
        self.assertError(bad % "window: 80ms", r"out of range \(1ms\.\.50ms\)")
        self.assertError(bad % "curve: hrad", r"did you mean 'hard'")
        self.assertError(bad % "channel: 17", r"17 is out of range \(0\.\.16\)")
        self.assertError(bad % "treshold: 300", r"did you mean 'threshold'")
        self.assertTrue(any("there are no pads: ignored" in w
                            for w in self.warns(MINIMAL_NUC + "pad_settings: { threshold: 300 }\n")))

    def test_pad_errors(self):
        self.assertError(PADS_STM + "  - { id: p3, pin: PA6 }\n", r"pads\[2\]\.note.*|is required", path="pads[2].note")
        self.assertError(PADS_STM + "  - { id: p3, pin: PC6, note: 40 }\n", r"PC6 has no ADC on stm32f1")
        self.assertError(PADS_STM + "  - { id: p3, pin: { cd74hc4051: m, number: 8 }, note: 40 }\n",
                         r"channel 8 is out of range")
        hc =PADS_STM.replace("pads:", "sn74hc165:\n  - { id: s, data_pin: PB0, clock_pin: PB1, load_pin: PB2 }\npads:")
        self.assertError(hc + "  - { id: p3, pin: { sn74hc165: s, number: 0 }, note: 40 }\n",
                         r"sn74hc165, which can't be an analog input.*a pad is read with the ADC")
        self.assertError(PADS_STM + "  - { id: p3, pin: PA6, note: 128 }\n", r"128 is out of range")
        self.assertError(PADS_STM + "  - { id: p3, pin: PA6, note: 40, cc: 3 }\n", r"unknown key")
        self.assertError(PADS_STM + "  - { id: kick, pin: PA6, note: 40 }\n", r"id 'kick' .* is already the id")
        self.assertError(PADS_STM + "  - { id: p3, pin: PA1, note: 40 }\n", r"PA1 is already used by pads\[1\]")
        self.assertTrue(any("note 36 is also the note of pads[0]" in w
                            for w in self.warns(PADS_STM + "  - { id: p3, pin: PA6, note: 36 }\n")))
        # a pad shares the select lines of another mux only with allow_other_uses on every use
        two = PADS_STM.replace("cd74hc4051:\n  - { id: m, pin_s0: PC0, pin_s1: PC1, pin_s2: PC2, common_pin: PA0 }",
                               "cd74hc4051:\n  - { id: m, pin_s0: PC0, pin_s1: PC1, pin_s2: PC2, common_pin: PA0 }\n"
                               "  - { id: m2, pin_s0: PC0, pin_s1: PC1, pin_s2: PC2, common_pin: PA2 }")
        self.assertError(two + "  - { id: p3, pin: { cd74hc4051: m2, number: 0 }, note: 40 }\n",
                         r"set allow_other_uses: true on each use")
        shared = two.replace("pin_s0: PC0, pin_s1: PC1, pin_s2: PC2",
                             "pin_s0: { number: PC0, allow_other_uses: true }, "
                             "pin_s1: { number: PC1, allow_other_uses: true }, "
                             "pin_s2: { number: PC2, allow_other_uses: true }")
        self.assertClean(shared + "  - { id: p3, pin: { cd74hc4051: m2, number: 0 }, note: 40 }\n")

    def test_presets(self):
        base = PADS_STM + "pad_settings: { channel: 10 }\npresets:\n  list:\n    - name: Default\n    - name: B\n"
        _, files = self.gen(base + "      pads: { snare: { note: 40 } }\n"
                                   "      pad_settings: { channel: 0, curve: hard, aftertouch: true }\n")
        c = files["gm_device_config.c"]
        for s in ("{ 20, 1, 40 }, /* pad.note[snare] = 40 */", "{ 21, 0, 0 }, /* pad.channel = 0 */",
                  "{ 22, 0, 2 }, /* pad.curve = hard */", "{ 23, 0, 1 }, /* pad.aftertouch = true */"):
            self.assertIn(s, c)
        self.assertTrue(any("the same as in preset 1 (pad.channel = 10)" in w
                            for w in self.warns(base + "      pad_settings: { channel: 10 }\n")))
        self.assertError(base + "      pad_settings: { threshold: 300 }\n",
                         r"global \(the pads' calibration.*set it in the top-level pad_settings:")
        self.assertError(base + "      pads: { snare: { pin: PA2 } }\n", r"pin: is the same in every preset")
        self.assertError(base + "      pads: { snar: { note: 40 } }\n", r"no pad with id 'snar'.*did you mean 'snare'")
        nopads = MINIMAL_NUC + "presets:\n  list:\n    - name: Default\n    - name: B\n"
        self.assertError(nopads + "      pad_settings: { curve: hard }\n",
                         r"pad.curve doesn't exist on this device: the device has no pads")

    def test_code61(self):
        dev, diags = gd.load_device(os.path.join(REPO, "devices", "m-audio-code61.yaml"))
        self.assertIsNotNone(dev, [str(d) for d in diags])
        self.assertEqual([p.id for p in dev.pads], [f"pad_{k}" for k in range(1, 17)])
        self.assertEqual([p.name for p in dev.pads], [f"Pad {k}" for k in range(1, 17)])
        hubs = {h.id: h.index + 1 for h in dev.hubs}
        self.assertEqual({p.pin.hub for p in dev.pads}, {hubs["pad_mux_a"], hubs["pad_mux_b"]})


# ---- encoder acceleration (core/README.md "Encoder acceleration") ----

ENC_NUC = MINIMAL_NUC + """\
encoders:
  - { id: e1, pin_a: PB6, pin_b: PB7, cc: 20 }
  - { id: e2, pin_a: PB8, pin_b: PB9, cc: 21 }
"""


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestEncoderAcceleration(unittest.TestCase):
    errs = TestValidation.errs
    assertError = TestValidation.assertError
    assertClean = TestValidation.assertClean

    def table(self, text):
        """The generated gm_enc_accel[] rows as (values, encoder id), or None without the table."""
        dev, diags = load(text)
        self.assertIsNotNone(dev, [str(d) for d in diags])
        self.assertFalse(gd.errors(diags), [str(d) for d in diags])
        files = gd.generate(dev, "t.yaml")
        h, c = files["gm_device_config.h"], files["gm_device_config.c"]
        on = "#define GM_ENC_ACCEL 1 " in h
        self.assertEqual(on, "gm_enc_accel[" in c)
        self.assertTrue(on or "#define GM_ENC_ACCEL 0 " in h)
        return re.findall(r"^    \{ (\d+, \d+, \d+, GM_ACCEL_\w+) \}, /\* (\w+): ", c, re.M) if on else None

    def enc(self, i, extra, base=ENC_NUC):
        """base with `extra` (e.g. 'acceleration: low') added to encoder i's mapping."""
        old = next(ln for ln in base.split("\n") if f"id: e{i + 1}," in ln)
        return base.replace(old, old[:-2] + f", {extra} }}")

    def test_off_by_default(self):
        self.assertIsNone(self.table(ENC_NUC))
        self.assertIsNone(self.table(self.enc(0, "acceleration: off")))
        self.assertIsNone(self.table(self.enc(0, "acceleration: 'off'")))
        self.assertIsNone(self.table(ENC_NUC + "encoder_settings: { acceleration: off }\n"))
        self.assertIsNone(self.table(ENC_NUC + "encoder_settings: {}\n"))
        self.assertIsNone(self.table(MINIMAL_NUC))          # no encoders at all: GM_ENC_ACCEL 0 still

    def test_presets(self):
        for name, row in (("low", "60, 15, 4, GM_ACCEL_LINEAR"), ("medium", "80, 10, 8, GM_ACCEL_QUADRATIC"),
                          ("high", "100, 8, 16, GM_ACCEL_QUADRATIC")):
            with self.subTest(name):
                self.assertEqual(self.table(self.enc(1, f"acceleration: {name}")),
                                 [("0, 0, 1, GM_ACCEL_LINEAR", "e1"), (row, "e2")])
        dev, _ = load(self.enc(0, "acceleration: medium"))
        c = gd.generate(dev, "t.yaml")["gm_device_config.c"]
        self.assertIn("{ 80, 10, 8, GM_ACCEL_QUADRATIC }, /* e1: medium */", c)
        self.assertIn("{ 0, 0, 1, GM_ACCEL_LINEAR }, /* e2: off */", c)

    def test_mapping(self):
        rows = self.table(self.enc(0, "acceleration: { slow: 120ms, fast: 5ms, max: 32, curve: cubic }"))
        self.assertEqual(rows[0], ("120, 5, 32, GM_ACCEL_CUBIC", "e1"))
        # partial: the rest from medium
        self.assertEqual(self.table(self.enc(0, "acceleration: { max: 4 }"))[0],
                         ("80, 10, 4, GM_ACCEL_QUADRATIC", "e1"))
        self.assertEqual(self.table(self.enc(0, "acceleration: { curve: linear, slow: 0.1s }"))[0],
                         ("100, 10, 8, GM_ACCEL_LINEAR", "e1"))
        dev, _ = load(self.enc(0, "acceleration: { max: 4 }"))
        self.assertIn("/* e1: slow 80ms, fast 10ms, max 4, quadratic */",
                      gd.generate(dev, "t.yaml")["gm_device_config.c"])

    def test_default_and_override(self):
        top = "encoder_settings: { acceleration: high }\n"
        self.assertEqual(self.table(ENC_NUC + top), [("100, 8, 16, GM_ACCEL_QUADRATIC", "e1"),
                                                     ("100, 8, 16, GM_ACCEL_QUADRATIC", "e2")])
        self.assertEqual(self.table(self.enc(1, "acceleration: off") + top),
                         [("100, 8, 16, GM_ACCEL_QUADRATIC", "e1"), ("0, 0, 1, GM_ACCEL_LINEAR", "e2")])
        self.assertEqual(self.table(self.enc(1, "acceleration: low") + top)[1],
                         ("60, 15, 4, GM_ACCEL_LINEAR", "e2"))
        # a partial mapping fills in from the top-level default, and the default's from medium
        self.assertEqual(self.table(self.enc(1, "acceleration: { max: 4 }") + top)[1],
                         ("100, 8, 4, GM_ACCEL_QUADRATIC", "e2"))
        self.assertEqual(self.table(ENC_NUC + "encoder_settings: { acceleration: { fast: 20ms } }\n")[0],
                         ("80, 20, 8, GM_ACCEL_QUADRATIC", "e1"))

    def test_twins_share_the_base(self):
        base = MODES_NUC.replace("  - { id: other, pin: PA3, action: note, note: 60 }\n",
                                 "  - { id: other, pin: PA3, action: note, note: 60 }\n"
                                 "  - { id: sel, pin: PA6 }\n") + \
            "encoders:\n  - { id: e1, pin_a: PB6, pin_b: PB7, cc: 20, acceleration: low }\n" \
            "  - { id: e2, pin_a: PB9, pin_b: PB10, cc: 21 }\n"
        text = modes("- id: grid\n  toggle: { button: sel }\n  layer:\n    e1: { cc: 30 }\n", base)
        self.assertEqual(self.table(text), [("60, 15, 4, GM_ACCEL_LINEAR", "e1"), ("0, 0, 1, GM_ACCEL_LINEAR", "e2")])
        dev, _ = load(text)
        self.assertIn("const gm_enc_accel_t gm_enc_accel[2] = {", gd.generate(dev, "t.yaml")["gm_device_config.c"])
        self.assertError(modes("- id: grid\n  toggle: { button: sel }\n  layer:\n    e1: { acceleration: high }\n",
                               base), r"acceleration: is the physical encoder's.*set it on the encoder")

    def test_bad_values(self):
        self.assertError(self.enc(0, "acceleration: meduim"),
                         r"unknown acceleration 'meduim' \(did you mean 'medium'\?", line=10,
                         path="encoders[0].acceleration")
        self.assertError(self.enc(0, "acceleration: 8"), r"must be off, low, medium, high or a mapping")
        self.assertError(self.enc(0, "acceleration: on"), r"must be off, low, medium, high or a mapping")
        self.assertError(self.enc(0, "acceleration: { slow: 80 }"), r"needs a unit")
        self.assertError(self.enc(0, "acceleration: { slow: 2s }"), r"out of range \(2ms\.\.1000ms\)")
        self.assertError(self.enc(0, "acceleration: { fast: 0.5ms }"), r"out of range")
        self.assertError(self.enc(0, "acceleration: { max: 33 }"), r"33 is out of range \(1\.\.32\)")
        self.assertError(self.enc(0, "acceleration: { max: 0 }"), r"0 is out of range \(1\.\.32\)")
        self.assertError(self.enc(0, "acceleration: { curve: quadric }"), r"did you mean 'quadratic'")
        self.assertError(self.enc(0, "acceleration: { slwo: 80ms }"), r"unknown key \(did you mean 'slow'\?\)")
        self.assertError(self.enc(0, "acceleration: { slow: 20ms, fast: 20ms }"),
                         r"fast \(20ms\) must be shorter than slow \(20ms\)")
        self.assertError(self.enc(0, "acceleration: { fast: 90ms }"),
                         r"fast \(90ms\) must be shorter than slow \(80ms\)")
        self.assertError(ENC_NUC + "encoder_settings: { acceleraton: low }\n", r"did you mean 'acceleration'")
        self.assertError(ENC_NUC + "encoder_settings: { acceleration: hgih }\n", r"did you mean 'high'",
                         path="encoder_settings.acceleration")
        self.assertError(ENC_NUC + "encoder_setting: { acceleration: low }\n", r"did you mean 'encoder_settings'")
        warns = [d.msg for d in self.errs(MINIMAL_NUC + "encoder_settings: { acceleration: low }\n",
                                          level="warning")]
        self.assertIn("there are no encoders: ignored", warns)
        text = self.enc(0, "acceleration: { max: 1 }")
        self.assertTrue(any("the same as off" in d.msg for d in self.errs(text, level="warning")))
        self.assertIsNone(self.table(text))
        # a preset can't set it: it is the encoder's
        pre = "presets:\n  slots: 2\n  list:\n    - name: A\n    - name: B\n      encoders: { e1: { acceleration: high } }\n"
        self.assertError(ENC_NUC + pre, r"acceleration: is the same in every preset")


@unittest.skipUnless(gd.yaml, "needs PyYAML")
class TestYamlReference(unittest.TestCase):
    """web/updater/src/builder/yaml-reference.json: the examples the web editor shows must validate, and the
    settings they name must be the builder's (PARAMS)."""
    REF = os.path.join(REPO, "web", "updater", "src", "builder", "yaml-reference.json")
    BASE = MINIMAL_NUC.replace("nuvoton-nuc1xx", "stm32f1").replace("NUC120LC1BN", "STM32F103RB").split("buttons:")[0]

    def test_examples_validate(self):
        with open(self.REF, encoding="utf-8") as f:
            entries = json.load(f)["entries"]
        self.assertTrue(entries)
        for e in entries:
            with self.subTest(e["id"]):
                dev, diags = load(self.BASE + e["yaml"])
                self.assertIsNotNone(dev, [str(d) for d in diags])
                self.assertEqual([str(d) for d in diags if d.level == "warning" and "has no inputs" not in d.msg], [])
                for name in e["params"]:
                    self.assertIn(name, gd.PARAM_ID)


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
        rc, out, err = self.run_cli("validate", KEYSTATION, bad)      # several files: 1 if any fails
        self.assertEqual(rc, 1)
        self.assertIn("presets: 8 slots, worst case 1908 B in 5 of 5 pages (2560 B)", out)
        self.assertIn("bad.yaml: 1 error", err)

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
        self.assertEqual(gb.peek_info(p)[3], "mine")              # and HELLO's node name
        rc, _, err = self.run_cli("new", "mine", "--preset", "keystation-mini32-mk3", "--dir", self.tmp)
        self.assertEqual(rc, 1)
        self.assertIn("already exists", err)
        rc, _, err = self.run_cli("new", "../evil", "--preset", "keystation-mini32-mk3", "--dir", self.tmp)
        self.assertEqual(rc, 1)
        rc, _, err = self.run_cli("new", "x", "--preset", "nope", "--dir", self.tmp)
        self.assertIn("unknown preset", err)


@unittest.skipUnless(shutil.which("openssl"), "needs openssl")
class TestDashboardTLS(unittest.TestCase):
    """A browser that rejects the self-signed certificate (the first visit) is logged in one line,
    with no traceback from the handler."""

    def test_rejected_certificate(self):
        import ssl
        import socket
        import dashboard
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        cert, key = os.path.join(tmp, "c.pem"), os.path.join(tmp, "k.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj",
                        "/CN=localhost", "-keyout", key, "-out", cert], check=True, capture_output=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        roots = dashboard.Roots(tmp, os.path.join(tmp, "build"), readonly_presets=True)
        err = io.StringIO()
        with redirect_stderr(err):
            srv, port = start_server(roots, tls=ctx)
            self.addCleanup(srv.shutdown)
            client = ssl.create_default_context()           # verifies: rejects the self-signed cert
            with socket.create_connection(("127.0.0.1", port), timeout=10) as raw:
                with self.assertRaises(ssl.SSLError):
                    client.wrap_socket(raw, server_hostname="localhost")
            time.sleep(0.3)                                 # let the handler thread finish
        self.assertNotIn("Traceback", err.getvalue())
        self.assertNotIn("Bad file descriptor", err.getvalue())


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
        devices = {d["id"]: d for d in json.loads(body)["devices"]}
        self.assertIn("presets/keystation-mini32-mk3.yaml", devices)
        self.assertEqual(devices["presets/keystation-mini32-mk3.yaml"]["node_name"], "keystation-mini32")
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
        with open(KEYSTATION) as f:
            st, body = self.req("/api/validate", {"text": f.read()})
        j = json.loads(body)
        self.assertTrue(j["ok"])
        self.assertIn("presets: 8 slots, worst case 1908 B in 5 of 5 pages", j["summary"])
        self.assertEqual([p["name"] for p in j["presets"]["slots"]][:3], ["Default", "Drums", "DAW"])
        self.assertEqual(j["presets"]["slots"][1]["changes"], 6)
        self.assertEqual(j["presets"]["storage"]["max_slots"], 9)
        self.assertIsNone(j["dma"])                                      # not stm32f1
        self.assertIsNone(j["din_midi"])
        self.assertEqual(j["midi_ports"], [])
        with open(os.path.join(REPO, "devices", "m-audio-code61.yaml"), encoding="utf-8") as f:
            st, body = self.req("/api/validate", {"text": f.read()})
        j = json.loads(body)
        self.assertTrue(j["ok"], j["diags"])
        self.assertEqual([c["channel"] for c in j["dma"]["channels"]], [1, 2, 3, 4, 5, 6, 7])
        self.assertEqual(j["dma"]["free"], [])
        self.assertIn({"channel": 7, "request": "USART2_TX", "user": "rgb_serial 'pad_leds'"}, j["dma"]["channels"])
        self.assertEqual(j["din_midi"], {"mode": "usb_merge", "midi_in": True, "thru": False,
                                         "controller_out": True, "usb_to_din": True, "dma": False})
        self.assertEqual(j["midi_ports"], [])                     # usb_merge: the main port only
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
        self.assertEqual((j["name"], j["version"], j["user_dir"]), ("gemifi builder", self.dashboard.VERSION, self.mine))
        self.assertEqual(set(j["toolchains"]), {"nuvoton-nuc1xx", "stm32f1", "esp"})
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
        ver = gd.load_device(KEYSTATION)[0].version
        stem = "keystation-mini32-" + ".".join(map(str, ver))
        self.assertEqual(imgs, [f"{stem}.bin", f"{stem}.gfw"])
        st, _h, data = raw_req(self.port, "GET", "/api/image?id=presets%2Fkeystation-mini32-mk3.yaml"
                               f"&file={stem}.gfw")
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
