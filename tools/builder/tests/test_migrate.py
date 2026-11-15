"""gemifi_build.py migrate (gemifi_migrate.py): old component keys rewritten to the current form."""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

TESTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(TESTS))
import gemifi_build as gb  # noqa: E402
import gemifi_device as gd  # noqa: E402
import gemifi_migrate as gm  # noqa: E402

HEAD = """\
gemifi: 2
name: test
board_id: 0x3F02
platform: esp32s3
chip: some-board
midi: { transport: serial }
"""

OLD = HEAD + """\
# the strip
ws2812:
  - { id: strip, pin: GPIO5, count: 4 }   # four LEDs
tm1628:
  - id: panel        # front panel
    clk_pin: GPIO12
    dio_pin: GPIO13
    stb_pin: GPIO14
i2c: { sda: GPIO21, scl: GPIO2 }
ssd1306:
  - { id: oled, address: 0x3C }
# the digits
max7219: [{ id: digits, clk_pin: GPIO18, din_pin: GPIO11, cs_pin: GPIO10 }]
leds:
  - { id: l0, pin: { ws2812: strip, number: 0 } }
  - id: l1
    pin:
      tm1628: panel
      number: 3
displays:
  - { id: top, type: text, ssd1306: oled, line: 0, shows: channel }
  - id: d2
    type: segment7
    chars:
      - { max7219: digits, number: 0 }
"""

NEW = HEAD + """\
# the strip
led_strip:
  - { id: strip, platform: ws2812, pin: GPIO5, count: 4 }   # four LEDs
led_driver:
  - id: panel        # front panel
    platform: tm1628
    clk_pin: GPIO12
    dio_pin: GPIO13
    stb_pin: GPIO14
  # the digits
  - { id: digits, platform: max7219, clk_pin: GPIO18, din_pin: GPIO11, cs_pin: GPIO10 }
i2c: { sda: GPIO21, scl: GPIO2 }
display:
  - { id: oled, platform: ssd1306, address: 0x3C }
leds:
  - { id: l0, pin: { led_strip: strip, number: 0 } }
  - id: l1
    pin:
      led_driver: panel
      number: 3
displays:
  - { id: top, type: text, display: oled, line: 0, shows: channel }
  - id: d2
    type: segment7
    chars:
      - { led_driver: digits, number: 0 }
"""


class Migrate(unittest.TestCase):
    def test_moves_from_the_components(self):
        """The table is the components' old_keys: every LED hub that is a platform now, and ssd1306."""
        self.assertEqual(gm.moves(), {
            "ws2812": ("led_strip", "ws2812"), "maudio_pad_leds": ("led_strip", "maudio_pads"),
            "rgb_serial": ("led_strip", "maudio_pads"), "tm1628": ("led_driver", "tm1628"),
            "max7219": ("led_driver", "max7219"), "tm1637": ("led_driver", "tm1637"),
            "ht16k33": ("led_driver", "ht16k33"), "ssd1306": ("display", "ssd1306")})

    def test_rewrites_lists_pins_and_screens(self):
        """Lists get the base's key and platform: on each item (flow and block), a second list of a base
        is merged into the first with its comments, pin keys and a text display's screen are renamed;
        comments stay."""
        old, _ = gd.load_device("t.yaml", OLD)
        self.assertIsNone(old)
        new, notes = gm.migrate(OLD)
        self.assertEqual(new, NEW)
        self.assertIn("line 8: ws2812: -> led_strip: with platform: ws2812", notes)
        self.assertTrue(any(n.startswith("merged a second led_driver: list") for n in notes))
        dev, diags = gd.load_device("t.yaml", new)
        self.assertIsNotNone(dev, [str(d) for d in diags if d.level == "error"])
        self.assertEqual([(h.id, h.type) for h in dev.hubs],
                         [("strip", "ws2812"), ("panel", "tm1628"), ("digits", "max7219")])

    def test_current_files_unchanged(self):
        """A file in the current form, and a second pass, change nothing."""
        self.assertEqual(gm.migrate(NEW), (NEW, []))
        devices = os.path.join(gb.REPO, "devices")
        for f in sorted(os.listdir(devices)):
            if f.endswith(".yaml"):
                with self.subTest(f), open(os.path.join(devices, f), encoding="utf-8") as fh:
                    text = fh.read()
                    self.assertEqual(gm.migrate(text)[0], text)

    def test_block_items_and_maudio_pads(self):
        old = HEAD.replace("esp32s3", "stm32f1").replace("some-board", "STM32F103RB").replace(
            "serial", "usb") + ("usb: { vid: 0x1209, pid: 0x0002 }\nmaudio_pad_leds:\n  -\n    id: pads\n"
                                "    tx_pin: PA2\n    count: 4\n")
        new, _ = gm.migrate(old)
        self.assertIn("led_strip:\n  -\n    platform: maudio_pads\n    id: pads\n", new)
        dev, diags = gd.load_device("t.yaml", new)
        self.assertIsNotNone(dev, [str(d) for d in diags if d.level == "error"])

    def test_multi_line_flow_is_left_with_a_note(self):
        old = HEAD + "ws2812: [\n  { id: strip, pin: GPIO5, count: 4 } ]\n"
        new, notes = gm.migrate(old)
        self.assertTrue(new.startswith(HEAD + "led_strip: [\n"))
        self.assertTrue(any("by hand" in n for n in notes), notes)

    def test_command(self):
        """migrate prints a diff; --write rewrites the file and checks it."""
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "old.yaml")
            with open(p, "w", encoding="utf-8") as f:
                f.write(OLD)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gb.main(["migrate", p]), 0)
            self.assertIn("+led_strip:", out.getvalue())
            with open(p, encoding="utf-8") as f:
                self.assertEqual(f.read(), OLD)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(gb.main(["migrate", "--write", p]), 0)
            with open(p, encoding="utf-8") as f:
                self.assertEqual(f.read(), NEW)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gb.main(["migrate", p]), 0)
            self.assertIn("nothing to migrate", out.getvalue())


if __name__ == "__main__":
    unittest.main()
